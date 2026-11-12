//
//  AirportQCAInterface.hpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//
//  IO80211Interface subclass bridging 802.11/Ethernet packets to macOS networking stack.
//

#ifndef _AIRPORT_QCA_INTERFACE_HPP_
#define _AIRPORT_QCA_INTERFACE_HPP_

#define __IO80211_TARGET 12

#include "Apple80211/Apple80211.h"
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <sys/kernel_types.h>

class AirportQCA;

class AirportQCAInterface : public IO80211Interface {
    OSDeclareDefaultStructors(AirportQCAInterface)

public:
    virtual UInt32 inputPacket(
        mbuf_t       packet,
        UInt32       length  = 0,
        IOOptionBits options = 0,
        void *       param   = 0) APPLE_KEXT_OVERRIDE;

    bool init(IO80211Controller *controller, AirportQCA *driver);
    void free() APPLE_KEXT_OVERRIDE;

private:
    AirportQCA *fDriver;
};

#endif /* _AIRPORT_QCA_INTERFACE_HPP_ */
