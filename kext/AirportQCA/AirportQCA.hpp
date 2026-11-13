//
//  AirportQCA.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  Main IO80211Controller driver class for Qualcomm Atheros QCA9377.
//

#ifndef _AIRPORT_QCA_HPP_
#define _AIRPORT_QCA_HPP_

#include "Apple80211/Apple80211.h"
#include "include/qca_compat.h"
#include "include/qca_hw.h"
#include "include/qca_regs.h"

#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <libkern/c++/OSString.h>

class AirportQCAInterface;
class AirportQCAEthernetInterface;
class QCAPCIDevice;

enum {
    kQCAPowerStateOff = 0,
    kQCAPowerStateOn  = 1,
    kQCAPowerStateCount
};

class AirportQCA : public IO80211Controller {
    OSDeclareDefaultStructors(AirportQCA)

public:
    virtual bool init(OSDictionary *properties = nullptr) APPLE_KEXT_OVERRIDE;
    virtual void free() APPLE_KEXT_OVERRIDE;

    virtual IOService* probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

#ifdef IO80211FAMILY_V2
    virtual IOReturn enable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
#else
    virtual IOReturn enable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disable(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;
#endif

    virtual IOReturn getHardwareAddress(IOEthernetAddress *addrP) APPLE_KEXT_OVERRIDE;
#ifndef IO80211FAMILY_V2
    virtual IOReturn setHardwareAddress(const IOEthernetAddress *addrP) APPLE_KEXT_OVERRIDE;
#else
    virtual IOReturn setHardwareAddress(const void *addr, UInt32 addrBytes) APPLE_KEXT_OVERRIDE;
#endif

    virtual UInt32 outputPacket(mbuf_t m, void *param) APPLE_KEXT_OVERRIDE;
    SInt32 dispatchApple80211Request(unsigned int requestType, int requestNumber, void *data);
#ifndef IO80211FAMILY_V2
    virtual int outputRaw80211Packet(IO80211Interface *interface, mbuf_t m) APPLE_KEXT_OVERRIDE;
    virtual bool useAppleRSNSupplicant(IO80211Interface *interface) APPLE_KEXT_OVERRIDE;
    virtual SInt32 apple80211Request(unsigned int request_type, int request_number,
                                    IO80211Interface *interface, void *data) APPLE_KEXT_OVERRIDE;
#endif

    virtual IONetworkInterface* createInterface() APPLE_KEXT_OVERRIDE;
    virtual bool configureInterface(IONetworkInterface *netif) APPLE_KEXT_OVERRIDE;

    virtual const OSString* newVendorString() const APPLE_KEXT_OVERRIDE;
    virtual const OSString* newModelString() const APPLE_KEXT_OVERRIDE;

    virtual IOWorkLoop* getWorkLoop() const APPLE_KEXT_OVERRIDE;
    virtual bool createWorkLoop() APPLE_KEXT_OVERRIDE;

    virtual bool setLinkStatus(
        UInt32 status,
        const IONetworkMedium *activeMedium = 0,
        UInt64 speed = 0,
        OSData *data = 0) APPLE_KEXT_OVERRIDE;

    virtual IOReturn setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) APPLE_KEXT_OVERRIDE;

#ifdef IO80211FAMILY_V2
    virtual bool isCommandProhibited(int command) APPLE_KEXT_OVERRIDE;
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *interface, unsigned long request,
                                     void *data, bool set) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *, apple80211_version_data *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *, apple80211_version_data *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *, apple80211_capability_data *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getPOWER(IO80211SkywalkInterface *, apple80211_power_data *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setPOWER(IO80211SkywalkInterface *, apple80211_power_data *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *) APPLE_KEXT_OVERRIDE;
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *, apple80211_debug_command *) APPLE_KEXT_OVERRIDE;
    virtual CCLogStream *getLogger() const APPLE_KEXT_OVERRIDE;
    virtual CommonFaultReporter *getFaultReporterFromDriver() APPLE_KEXT_OVERRIDE;
    virtual bool requiresExplicitMBufRelease() APPLE_KEXT_OVERRIDE { return false; }
    virtual bool flowIdSupported() APPLE_KEXT_OVERRIDE { return false; }
    virtual bool getLogPipes(CCPipe **, CCPipe **, CCPipe **) APPLE_KEXT_OVERRIDE { return false; }
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *, unsigned short *) APPLE_KEXT_OVERRIDE { return kIOReturnSuccess; }
    virtual UInt32 hardwareOutputQueueDepth() APPLE_KEXT_OVERRIDE;
    virtual SInt32 performCountryCodeOperation(IO80211CountryCodeOp) APPLE_KEXT_OVERRIDE;
    virtual SInt32 monitorModeSetEnabled(bool enabled, unsigned int channel) APPLE_KEXT_OVERRIDE;
#else
    /* Legacy IO80211Controller hooks retained for non-Tahoe builds. */
    virtual SInt32 stopDMA() APPLE_KEXT_OVERRIDE;
    virtual UInt32 hardwareOutputQueueDepth(IO80211Interface *interface) APPLE_KEXT_OVERRIDE;
    virtual SInt32 performCountryCodeOperation(IO80211Interface *interface, IO80211CountryCodeOp op) APPLE_KEXT_OVERRIDE;

    /* IO80211Controller virtual overrides (non-pure, but needed for concrete instantiation) */
    virtual int bpfOutputPacket(OSObject *obj, UInt type, mbuf_t m) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getHardwareAddressForInterface(IO80211Interface *interface, IOEthernetAddress *addr) APPLE_KEXT_OVERRIDE;
    virtual SInt32 monitorModeSetEnabled(IO80211Interface *interface, bool enabled, UInt channel) APPLE_KEXT_OVERRIDE;
    virtual void requestPacketTx(void *data, UInt length) APPLE_KEXT_OVERRIDE;
    virtual SInt32 enableVirtualInterface(IO80211VirtualInterface *vif) APPLE_KEXT_OVERRIDE;
    virtual SInt32 disableVirtualInterface(IO80211VirtualInterface *vif) APPLE_KEXT_OVERRIDE;
    virtual IO80211VirtualInterface* createVirtualInterface(ether_addr *addr, UInt role) APPLE_KEXT_OVERRIDE;
    virtual SInt32 apple80211VirtualRequest(UInt request_type, int request_number, IO80211VirtualInterface *vif, void *data) APPLE_KEXT_OVERRIDE;
#endif
    virtual SInt32 enableFeature(IO80211FeatureCode code, void *data) APPLE_KEXT_OVERRIDE;

    /* Hardware & interrupt handling */
    static bool interruptFilter(OSObject *owner, IOFilterInterruptEventSource *src);
    static void interruptAction(OSObject *owner, IOInterruptEventSource *src, int count);
    void handleHardwareInterrupt();

    /* Watchdog & Periodic Timers */
    void watchdogAction(IOTimerEventSource *timer);
    void notifyScanComplete();
    IOReturn prepareSkywalkBSDInterface(ifnet_t interface);
    AirportQCAInterface *getSkywalkInterface() const { return fNetIf; }

private:
    bool createMediumTables(const IONetworkMedium **primary);
    bool initPCIPowerManagement(IOPCIDevice *pci);
    void interfaceSetChannel(UInt32 channel);
    void interfaceSetPower(bool enabled);

    IOPCIDevice               *fPciNub;
    AirportQCAInterface       *fNetIf;
    AirportQCAEthernetInterface *fBSDInterface;
    IOWorkLoop                *fWorkLoop;
    IOCommandGate             *fCommandGate;
    IOFilterInterruptEventSource *fInterruptSource;
    IOTimerEventSource        *fWatchdogTimer;

    IOEthernetAddress          fMacAddress;
    uint32_t                   fPowerState;
    bool                       fLinkActive;
    uint32_t                   fChannel;
    int32_t                    fRssi;
    int32_t                    fNoise;

    /* QCA Hardware state */
    void                      *fHalDevice;
};

#endif /* _AIRPORT_QCA_HPP_ */
