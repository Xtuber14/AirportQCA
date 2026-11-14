//
//  QCAPCIProvider.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCAPCIProvider.hpp"
#include "Apple80211/IO80211WorkQueue.h"
#include "include/qca_compat.h"
#include "include/qca_hw.h"

#define super IOService
OSDefineMetaClassAndStructors(QCAPCIProvider, IOService)

bool QCAPCIProvider::init(OSDictionary *properties)
{
    if (!super::init(properties)) {
        return false;
    }

    pciNub = nullptr;
    workQueue = nullptr;

    qca_info("QCAPCIProvider initializing");
    return true;
}

void QCAPCIProvider::free()
{
    /* start() may fail before stop() ever runs, so release the queue here
       too; stop() clears the pointer first, making this a no-op then. */
    OSSafeReleaseNULL(workQueue);
    super::free();
}

IOService* QCAPCIProvider::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score)) {
        return nullptr;
    }

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        return nullptr;
    }

    UInt16 vendorId = pci->configRead16(kIOPCIConfigVendorID);
    UInt16 deviceId = pci->configRead16(kIOPCIConfigDeviceID);

    if (vendorId == QCA9377_PCI_VENDOR_ID && deviceId == QCA9377_1_0_DEVICE_ID) {
        qca_info("QCAPCIProvider: matched Qualcomm Atheros QCA9377 (%04x:%04x)",
                 vendorId, deviceId);
        pciNub = pci;
        return this;
    }

    return nullptr;
}

bool QCAPCIProvider::start(IOService *provider)
{
    /* This personality starts at the early boot stage (direct IOPCIDevice
       match). Create the IO80211WorkQueue now: the AirportQCA controller
       shares it and the WCL fault reporter runs on it, matching itlwm's
       IOPCIEDeviceWrapper. The controller itself waits on the
       IOResourceMatch=IOBSD gate, so its WCL setup happens late. */
    workQueue = IO80211WorkQueue::workQueue();
    if (!workQueue) {
        qca_err("IO80211WorkQueue creation failed");
        return false;
    }

    if (!super::start(provider)) {
        OSSafeReleaseNULL(workQueue);
        return false;
    }

    setProperty("QCAStartStage", "provider-started");
    registerService();
    return true;
}

void QCAPCIProvider::stop(IOService *provider)
{
    OSSafeReleaseNULL(workQueue);
    super::stop(provider);
}

IOWorkLoop* QCAPCIProvider::getWorkLoop() const
{
    return workQueue;
}
