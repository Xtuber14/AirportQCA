#include "AirportQCAEthernetInterface.hpp"
#include "AirportQCA.hpp"
#include <net/if_var.h>
#define super IOEthernetInterface
OSDefineMetaClassAndStructors(AirportQCAEthernetInterface, IOEthernetInterface)

bool AirportQCAEthernetInterface::initWithSkywalkInterface(IOEthernetController *controller, IO80211SkywalkInterface *interface)
{
    if (!super::init(controller)) return false;
    fSkywalkInterface = interface;
    fAttachedToSkywalk = false;
    return true;
}

IOReturn AirportQCAEthernetInterface::attachToDataLinkLayer(IOOptionBits options, void *parameter)
{
    IOReturn result = super::attachToDataLinkLayer(options, parameter);
    if (result != kIOReturnSuccess || !fSkywalkInterface || !getIfnet()) return result;
    AirportQCA *driver = OSDynamicCast(AirportQCA, getController());
    result = driver ? driver->prepareSkywalkBSDInterface(getIfnet()) : kIOReturnNotReady;
    if (result == kIOReturnSuccess) {
        fSkywalkInterface->registerService();
        fAttachedToSkywalk = true;
    }
    return result;
}

bool AirportQCAEthernetInterface::setLinkState(IO80211LinkState state)
{
    if (!getIfnet()) return false;
    const unsigned int flags = state == kIO80211NetworkLinkUp ? (IFF_UP | IFF_RUNNING) : 0;
    ifnet_set_flags(getIfnet(), flags, IFF_UP | IFF_RUNNING);
    return true;
}

void AirportQCAEthernetInterface::detachFromDataLinkLayer(IOOptionBits options, void *parameter)
{
    fAttachedToSkywalk = false;
    super::detachFromDataLinkLayer(options, parameter);
}

IOService *AirportQCAEthernetInterface::getProvider() const
{
    return fAttachedToSkywalk && fSkywalkInterface ? fSkywalkInterface : super::getProvider();
}
