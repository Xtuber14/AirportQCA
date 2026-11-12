//
//  AirportQCAInterface.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "AirportQCAInterface.hpp"
#include "AirportQCA.hpp"

#define super IO80211Interface
OSDefineMetaClassAndStructors(AirportQCAInterface, IO80211Interface)

bool AirportQCAInterface::init(IO80211Controller *controller, AirportQCA *driver)
{
    if (!super::init(controller)) {
        return false;
    }
    fDriver = driver;
    return true;
}

void AirportQCAInterface::free()
{
    fDriver = nullptr;
    super::free();
}

UInt32 AirportQCAInterface::inputPacket(mbuf_t packet, UInt32 length, IOOptionBits options, void *param)
{
    if (!packet) {
        return 0;
    }
    return super::inputPacket(packet, length, options, param);
}
