//
//  HyperVVMBusDevice.cpp
//  Hyper-V VMBus device nub
//
//  Copyright © 2021 Goldfish64. All rights reserved.
//

#include "HyperVVMBusDevice.hpp"
#include "HyperVVMBusDeviceInternal.hpp"

OSDefineMetaClassAndStructors(HyperVVMBusDevice, super);

bool HyperVVMBusDevice::attach(IOService *provider) {
  char channelLocation[10];
  
  if (!super::attach(provider)) {
    return false;
  }
  
  channelIsOpen = false;
  
  //
  // Get channel number.
  //
  OSNumber *channelNumber = OSDynamicCast(OSNumber, getProperty(kHyperVVMBusDeviceChannelIDKey));
  vmbusProvider = OSDynamicCast(HyperVVMBusController, getProvider());
  if (channelNumber == NULL || vmbusProvider == NULL) {
    return false;
  }
  channelId = channelNumber->unsigned32BitValue();
  DBGLOG("Attaching nub for channel %u", channelId);
  
  //
  // Set location to ensure unique names in I/O Registry.
  //
  snprintf(channelLocation, sizeof (channelLocation), "%x", channelId);
  setLocation(channelLocation);
  
  return true;
}

void HyperVVMBusDevice::detach(IOService *provider) {
  //
  // Close and free channel.
  //
  if (channelIsOpen) {
    closeChannel();
  }
  vmbusProvider->freeVMBusChannel(channelId);
  
  super::detach(provider);
}

bool HyperVVMBusDevice::openChannel(UInt32 txSize, UInt32 rxSize, OSObject *owner, IOInterruptEventAction intAction) {
  if (channelIsOpen) {
    return true;
  }
  
  DBGLOG("Opening channel for %u", channelId);
  txBufferSize = txSize;
  rxBufferSize = rxSize;
  
  if (!setupInterrupt()) {
    return false;
  }
  
  if (owner != NULL && intAction != NULL) {
    childInterruptSource = IOInterruptEventSource::interruptEventSource(owner, intAction);
    if (childInterruptSource == NULL) {
      return kIOReturnError;
    }

    workLoop->addEventSource(childInterruptSource);
    childInterruptSource->enable();
  }
  
  //
  // Open channel.
  //
  if (!vmbusProvider->initVMBusChannel(channelId, txBufferSize, &txBuffer, rxBufferSize, &rxBuffer)) {
    teardownInterrupt();
    return false;
  }
  
  if (!vmbusProvider->openVMBusChannel(channelId)) {
    vmbusProvider->closeVMBusChannel(channelId);
    teardownInterrupt();
    return false;
  }
  
  channelIsOpen = true;
  DBGLOG("Opened channel for %u", channelId);
  return true;
}

void HyperVVMBusDevice::closeChannel() {
  //
  // Close channel and stop interrupts.
  //
  vmbusProvider->closeVMBusChannel(channelId);
  teardownInterrupt();
  channelIsOpen = false;
}

bool HyperVVMBusDevice::createGpadlBuffer(UInt32 bufferSize, UInt32 *gpadlHandle, void **buffer) {
  return vmbusProvider->initVMBusChannelGpadl(channelId, bufferSize, gpadlHandle, buffer);
}

bool HyperVVMBusDevice::nextPacketAvailable(VMBusPacketType *type, UInt32 *packetHeaderLength, UInt32 *packetTotalLength) {
  return commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::nextPacketAvailableGated),
                                type, packetHeaderLength, packetTotalLength) == kIOReturnSuccess;
}

bool HyperVVMBusDevice::nextInbandPacketAvailable(UInt32 *packetDataLength) {
  VMBusPacketType pktType;
  UInt32 pktHeaderLength;
  UInt32 pktTotalLength;
  
  if (packetDataLength == NULL) {
    return false;
  }

  bool result = commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::nextPacketAvailableGated),
                                       &pktType, &pktHeaderLength, &pktTotalLength) == kIOReturnSuccess;
  
  if (result) {
    if (pktType == kVMBusPacketTypeDataInband) {
      *packetDataLength = pktTotalLength - pktHeaderLength;
    } else {
      result = false;
    }
  }
  return result;
}

IOReturn HyperVVMBusDevice::doRequest(HyperVVMBusDeviceRequest *request) {
  return commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::doRequestGated), request, NULL, NULL);
}

IOReturn HyperVVMBusDevice::readRawPacket(void *buffer, UInt32 bufferLength) {
  return commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::readRawPacketGated),
                                buffer, &bufferLength);
}

IOReturn HyperVVMBusDevice::readInbandPacket(void *buffer, UInt32 bufferLength, UInt64 *transactionId) {
  return commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::readInbandPacketGated),
                                buffer, &bufferLength, transactionId);
}

IOReturn HyperVVMBusDevice::writeRawPacket(void *buffer, UInt32 bufferLength) {
  return commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::writeRawPacketGated),
                                NULL, NULL, buffer, &bufferLength);
}

IOReturn HyperVVMBusDevice::writeInbandPacket(void *buffer, UInt32 bufferLength, bool responseRequired, UInt64 transactionId) {
  return commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::writeInbandPacketGated),
                                buffer, &bufferLength, &responseRequired, &transactionId);
}

IOReturn HyperVVMBusDevice::writeGPADirectSinglePagePacket(void *buffer, UInt32 bufferLength, bool responseRequired, UInt64 transactionId,
                                                           VMBusSinglePageBuffer pageBuffers[], UInt32 pageBufferCount) {
  if (pageBufferCount > kVMBusMaxPageBufferCount) {
    return kIOReturnNoResources;
  }

  //
  // Create packet for page buffers.
  //
  VMBusPacketSinglePageBuffer pagePacket;
  UInt32 pagePacketLength = sizeof (VMBusPacketSinglePageBuffer) -
    ((kVMBusMaxPageBufferCount - pageBufferCount) * sizeof (VMBusSinglePageBuffer));

  pagePacket.header.type          = kVMBusPacketTypeDataUsingGPADirect;
  pagePacket.header.headerLength  = pagePacketLength >> kVMBusPacketSizeShift;
  pagePacket.header.totalLength   = (pagePacketLength + bufferLength) >> kVMBusPacketSizeShift;
  pagePacket.header.flags         = responseRequired ? kVMBusPacketResponseRequired : 0;
  pagePacket.header.transactionId = transactionId;

  pagePacket.reserved             = 0;
  pagePacket.rangeCount           = pageBufferCount;
  for (int i = 0; i < pagePacket.rangeCount; i++) {
    pagePacket.ranges[i].length = pageBuffers[i].length;
    pagePacket.ranges[i].offset = pageBuffers[i].offset;
    pagePacket.ranges[i].pfn    = pageBuffers[i].pfn;
  }

  return commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::writeRawPacketGated),
                                &pagePacket, &pagePacketLength, buffer, &bufferLength);
}

IOReturn HyperVVMBusDevice::sendMessage(void *message, UInt32 messageLength, VMBusPacketType type, UInt64 transactionId,
                                        bool responseRequired, void *response, UInt32 *responseLength) {
  HyperVVMBusDeviceRequest request;
  memset(&request, 0, sizeof (request));
  
  request.sendData = message;
  request.sendDataLength = messageLength;
  request.sendPacketType = type;
  request.transactionId = transactionId;
  
  request.responseRequired = responseRequired;
  if (responseRequired && response && responseLength) {
    request.responseData = response;
    request.responseDataLength = *responseLength;
  }
  
  auto status = commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::doRequestGated), &request, NULL, NULL);
  
  if (responseRequired && response && responseLength) {
    *responseLength = request.responseDataLength;
  }
  return status;
}

IOReturn HyperVVMBusDevice::sendMessageSinglePageBuffers(void *message, UInt32 messageLength, UInt64 transactionId,
                                                         VMBusSinglePageBuffer pageBuffers[], UInt32 pageBufferCount,
                                                         bool responseRequired, void *response, UInt32 *responseLength) {
  if (pageBufferCount > kVMBusMaxPageBufferCount) {
    return kIOReturnNoResources;
  }
  
  HyperVVMBusDeviceRequest request;
  memset(&request, 0, sizeof (request));
  
  request.sendData = message;
  request.sendDataLength = messageLength;
  request.sendPacketType = kVMBusPacketTypeDataUsingGPADirect;
  request.transactionId = transactionId;
  
  request.responseRequired = responseRequired;
  if (responseRequired && response && responseLength) {
    request.responseData = response;
    request.responseDataLength = *responseLength;
  }
  
  // Create packet header for page buffers.
  VMBusPacketSinglePageBuffer pagePacket;
  pagePacket.reserved = 0;
  pagePacket.rangeCount = pageBufferCount;
  
  for (int i = 0; i < pagePacket.rangeCount; i++) {
    pagePacket.ranges[i].length = pageBuffers[i].length;
    pagePacket.ranges[i].offset = pageBuffers[i].offset;
    pagePacket.ranges[i].pfn    = pageBuffers[i].pfn;
  }
  UInt32 pagePacketLength = sizeof (VMBusPacketSinglePageBuffer) -
    ((kVMBusMaxPageBufferCount - pageBufferCount) * sizeof (VMBusSinglePageBuffer));
  
  auto status = commandGate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &HyperVVMBusDevice::doRequestGated),
                                       &request, &pagePacket, &pagePacketLength);
  if (responseRequired && response && responseLength) {
    *responseLength = request.responseDataLength;
  }
  return status;
}
