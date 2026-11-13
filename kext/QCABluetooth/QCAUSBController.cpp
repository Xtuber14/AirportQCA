//
//  QCAUSBController.cpp
//  QCABluetoothFirmware - Qualcomm Atheros QCA9377 Bluetooth Firmware Injector
//

#include "QCAUSBController.hpp"
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#define super OSObject
OSDefineMetaClassAndStructors(QCAUSBController, OSObject)

bool QCAUSBController::initWithDevice(IOService *client, IOUSBHostDevice *dev)
{
    if (!dev || !client) return false;

    fClient = client;
    fDevice = dev;
    fInterface = nullptr;
    fBulkOutPipe = nullptr;

    fDevice->retain();
    return true;
}

void QCAUSBController::free()
{
    if (fBulkOutPipe) {
        fBulkOutPipe->release();
        fBulkOutPipe = nullptr;
    }
    if (fInterface) {
        if (fInterface->isOpen(fClient)) {
            fInterface->close(fClient);
        }
        fInterface->release();
        fInterface = nullptr;
    }
    if (fDevice) {
        fDevice->release();
        fDevice = nullptr;
    }
    super::free();
}

IOReturn QCAUSBController::sendVendorRequestIn(uint8_t request, void *data, uint16_t size)
{
    if (!fDevice) return kIOReturnNotAttached;

    StandardUSB::DeviceRequest req;
    req.bmRequestType = 0xC0; /* Vendor | In */
    req.bRequest = request;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = size;

    uint32_t bytesTransferred = 0;
    return fDevice->deviceRequest(fClient, req, data, bytesTransferred, 2000);
}

IOReturn QCAUSBController::sendVendorRequestOut(uint8_t request, const void *data, uint16_t size)
{
    if (!fDevice) return kIOReturnNotAttached;

    StandardUSB::DeviceRequest req;
    req.bmRequestType = 0x40; /* Vendor | Out */
    req.bRequest = request;
    req.wValue = 0;
    req.wIndex = 0;
    req.wLength = size;

    uint32_t bytesTransferred = 0;
    return fDevice->deviceRequest(fClient, req, const_cast<void*>(data), bytesTransferred, 2000);
}

IOReturn QCAUSBController::bulkWrite(const void *data, uint32_t length, uint32_t timeoutMs)
{
    if (!fBulkOutPipe) {
        /* If bulk pipe not yet opened, open default bulk pipe */
        return kIOReturnNotOpen;
    }

    IOMemoryDescriptor *desc = IOBufferMemoryDescriptor::withBytes(data, length, kIODirectionOut);
    if (!desc) {
        return kIOReturnNoMemory;
    }

    uint32_t transferred = 0;
    IOReturn ret = fBulkOutPipe->io(desc, length, transferred, timeoutMs);
    desc->release();
    return ret;
}

bool QCAUSBController::resetController()
{
    /*
     * For Qualcomm Rome 3.2 (QCA9377), do NOT call fDevice->reset().
     * An IOUSBHostDevice hardware reset drops the USB physical connection,
     * causes re-enumeration on the USB root hub, destroys IOKit state,
     * and forces an infinite boot loop.
     * The Rome controller transitions into the patched execution state
     * autonomously after the NVM download completes.
     */
    return true;
}
