#ifndef AIRPORT_QCA_ETHERNET_INTERFACE_HPP
#define AIRPORT_QCA_ETHERNET_INTERFACE_HPP

#include "Apple80211/Apple80211.h"
#include <IOKit/network/IOEthernetInterface.h>

class AirportQCAEthernetInterface : public IOEthernetInterface {
    OSDeclareDefaultStructors(AirportQCAEthernetInterface)
public:
    bool initWithSkywalkInterface(IOEthernetController *controller, IO80211SkywalkInterface *interface);
    IOReturn attachToDataLinkLayer(IOOptionBits options, void *parameter) APPLE_KEXT_OVERRIDE;
    void detachFromDataLinkLayer(IOOptionBits options, void *parameter) APPLE_KEXT_OVERRIDE;
    bool setLinkState(IO80211LinkState state);
    IOService *getProvider() const APPLE_KEXT_OVERRIDE;
    // Tahoe IONetworkInterface reserved slots 5-10 replaced in-place:
    virtual void _RESERVEDIONetworkInterface5() __asm__("__ZN18IONetworkInterface24configureOutputPullModelEjjjjj") APPLE_KEXT_OVERRIDE;
    virtual void _RESERVEDIONetworkInterface6() __asm__("__ZN18IONetworkInterface27configureInputPacketPollingEjj") APPLE_KEXT_OVERRIDE;
    virtual void _RESERVEDIONetworkInterface7() __asm__("__ZN18IONetworkInterface23reportDataTransferRatesEyyyy") APPLE_KEXT_OVERRIDE;
    virtual void _RESERVEDIONetworkInterface8() __asm__("__ZN18IONetworkInterface20dequeueOutputPacketsEjPP6__mbufS2_PjPy") APPLE_KEXT_OVERRIDE;
    virtual void _RESERVEDIONetworkInterface9() __asm__("__ZN18IONetworkInterface36dequeueOutputPacketsWithServiceClassEj18IOMbufServiceClassPP6__mbufS3_PjPy") APPLE_KEXT_OVERRIDE;
    virtual void _RESERVEDIONetworkInterface10() __asm__("__ZN18IONetworkInterface18enqueueInputPacketEP6__mbufP11IOMbufQueuej") APPLE_KEXT_OVERRIDE;
private:
    IO80211SkywalkInterface *fSkywalkInterface;
    bool fAttachedToSkywalk;
};

#endif
