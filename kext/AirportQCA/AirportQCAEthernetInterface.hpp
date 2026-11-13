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
private:
    IO80211SkywalkInterface *fSkywalkInterface;
    bool fAttachedToSkywalk;
};

#endif
