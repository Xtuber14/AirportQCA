//
//  QCAPCIProvider.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Early-starting PCI provider: matches the QCA9377 IOPCIDevice nub, then
//  hands it to the AirportQCA controller, whose personality is gated on
//  IOResourceMatch=IOBSD so the WCL/Skywalk environment exists by the time
//  the controller starts. Mirrors itlwm's IOPCIEDeviceWrapper.
//

#ifndef _QCA_PCI_PROVIDER_HPP_
#define _QCA_PCI_PROVIDER_HPP_

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

class IO80211WorkQueue;

class QCAPCIProvider : public IOService {
    OSDeclareDefaultStructors(QCAPCIProvider)

public:
    virtual bool init(OSDictionary *properties = nullptr) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    virtual IOService* probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual IOWorkLoop* getWorkLoop() const APPLE_KEXT_OVERRIDE;

    /* Set in probe(), consumed by AirportQCA::probe/start. Not retained:
       the PCI nub outlives this provider in the registry. */
    IOPCIDevice *pciNub;

    /* IO80211WorkQueue created in start() at the early boot stage and
       shared with the AirportQCA controller. */
    IO80211WorkQueue *workQueue;
};

#endif /* _QCA_PCI_PROVIDER_HPP_ */
