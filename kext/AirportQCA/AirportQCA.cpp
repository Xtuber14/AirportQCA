//
//  AirportQCA.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "AirportQCA.hpp"
#include "AirportQCAInterface.hpp"
#include "hw/QCAPCIDevice.hpp"

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportQCA, IO80211Controller)

#define kWatchDogPeriodMs 1000

bool AirportQCA::init(OSDictionary *properties)
{
    if (!super::init(properties)) {
        return false;
    }

    fPciNub = nullptr;
    fNetIf = nullptr;
    fWorkLoop = nullptr;
    fCommandGate = nullptr;
    fInterruptSource = nullptr;
    fWatchdogTimer = nullptr;
    fHalDevice = nullptr;

    fPowerState = kQCAPowerStateOff;
    fLinkActive = false;
    fChannel = 1;
    fRssi = -50;
    fNoise = -95;

    /* Default fallback MAC address (overridden by hardware BDF / OTP during boot) */
    fMacAddress.bytes[0] = 0x00;
    fMacAddress.bytes[1] = 0x03;
    fMacAddress.bytes[2] = 0x7F;
    fMacAddress.bytes[3] = 0x11;
    fMacAddress.bytes[4] = 0x22;
    fMacAddress.bytes[5] = 0x33;

    qca_info("AirportQCA initializing");
    return true;
}

void AirportQCA::free()
{
    if (fHalDevice) {
        delete static_cast<QCAPCIDevice*>(fHalDevice);
        fHalDevice = nullptr;
    }
    super::free();
}

IOService* AirportQCA::probe(IOService *provider, SInt32 *score)
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
        qca_info("probe: matched Qualcomm Atheros QCA9377 (%04x:%04x)", vendorId, deviceId);
        *score += 2000;
        return this;
    }

    return nullptr;
}

bool AirportQCA::createWorkLoop()
{
    if (!fWorkLoop) {
        fWorkLoop = IOWorkLoop::workLoop();
    }
    return (fWorkLoop != nullptr);
}

IOWorkLoop* AirportQCA::getWorkLoop() const
{
    return fWorkLoop;
}

bool AirportQCA::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }

    fPciNub = OSDynamicCast(IOPCIDevice, provider);
    if (!fPciNub) {
        super::stop(provider);
        return false;
    }

    /* Enable Bus Master, Memory, and IO decoding */
    fPciNub->setBusMasterEnable(true);
    fPciNub->setMemoryEnable(true);
    fPciNub->setIOEnable(true);

    if (!createWorkLoop()) {
        qca_err("Failed to create workloop");
        super::stop(provider);
        return false;
    }

    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        qca_err("Failed to create command gate");
        super::stop(provider);
        return false;
    }
    fWorkLoop->addEventSource(fCommandGate);

    const IONetworkMedium *primaryMedium = nullptr;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) ||
        !setSelectedMedium(primaryMedium)) {
        qca_err("Failed to setup network mediums");
        super::stop(provider);
        return false;
    }

    /* Initialize hardware layer */
    auto *hal = new QCAPCIDevice();
    if (!hal || !hal->init(fPciNub, this)) {
        qca_err("Failed to initialize QCAPCIDevice HAL");
        if (hal) delete hal;
        super::stop(provider);
        return false;
    }
    fHalDevice = hal;

    /* Retrieve hardware MAC address */
    hal->getMacAddress(fMacAddress.bytes);

    /* Attach network interface */
    if (!attachInterface((IONetworkInterface **)&fNetIf, true)) {
        qca_err("Failed to attach IO80211Interface");
        super::stop(provider);
        return false;
    }

    /* Setup Watchdog Timer */
    fWatchdogTimer = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportQCA::watchdogAction)
    );
    if (fWatchdogTimer) {
        fWorkLoop->addEventSource(fWatchdogTimer);
        fWatchdogTimer->setTimeoutMS(kWatchDogPeriodMs);
    }

    /* Register power management */
    initPCIPowerManagement(fPciNub);

    setLinkStatus(kIONetworkLinkValid);
    registerService();
    fNetIf->registerService();

    qca_info("AirportQCA started successfully with native AirPort interface");
    return true;
}

void AirportQCA::stop(IOService *provider)
{
    if (fWatchdogTimer) {
        fWatchdogTimer->cancelTimeout();
        if (fWorkLoop) fWorkLoop->removeEventSource(fWatchdogTimer);
        fWatchdogTimer->release();
        fWatchdogTimer = nullptr;
    }

    if (fCommandGate) {
        if (fWorkLoop) fWorkLoop->removeEventSource(fCommandGate);
        fCommandGate->release();
        fCommandGate = nullptr;
    }

    if (fInterruptSource) {
        fInterruptSource->disable();
        if (fWorkLoop) fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = nullptr;
    }

    if (fNetIf) {
        detachInterface(fNetIf, true);
        fNetIf->release();
        fNetIf = nullptr;
    }

    if (fHalDevice) {
        auto *hal = static_cast<QCAPCIDevice*>(fHalDevice);
        hal->stop();
        delete hal;
        fHalDevice = nullptr;
    }

    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = nullptr;
    }

    super::stop(provider);
}

IONetworkInterface* AirportQCA::createInterface()
{
    AirportQCAInterface *netif = new AirportQCAInterface;
    if (!netif) {
        return nullptr;
    }
    if (!netif->init(this, this)) {
        netif->release();
        return nullptr;
    }
    return netif;
}

bool AirportQCA::configureInterface(IONetworkInterface *netif)
{
    if (!super::configureInterface(netif)) {
        return false;
    }
    return true;
}

IOReturn AirportQCA::enable(IONetworkInterface *netif)
{
    qca_info("enable adapter interface");
    if (fHalDevice) {
        static_cast<QCAPCIDevice*>(fHalDevice)->start();
    }
    setLinkStatus(kIONetworkLinkValid | kIONetworkLinkActive);
    fLinkActive = true;
    return kIOReturnSuccess;
}

IOReturn AirportQCA::disable(IONetworkInterface *netif)
{
    qca_info("disable adapter interface");
    if (fHalDevice) {
        static_cast<QCAPCIDevice*>(fHalDevice)->stop();
    }
    setLinkStatus(kIONetworkLinkValid);
    fLinkActive = false;
    return kIOReturnSuccess;
}

IOReturn AirportQCA::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (!addrP) return kIOReturnBadArgument;
    memcpy(addrP->bytes, fMacAddress.bytes, sizeof(fMacAddress.bytes));
    return kIOReturnSuccess;
}

IOReturn AirportQCA::setHardwareAddress(const IOEthernetAddress *addrP)
{
    if (!addrP) return kIOReturnBadArgument;
    memcpy(fMacAddress.bytes, addrP->bytes, sizeof(fMacAddress.bytes));
    return kIOReturnSuccess;
}

UInt32 AirportQCA::outputPacket(mbuf_t m, void *param)
{
    if (!m) return 0;
    if (fHalDevice && fLinkActive) {
        return static_cast<QCAPCIDevice*>(fHalDevice)->transmitPacket(m);
    }
    freePacket(m);
    return 0;
}

int AirportQCA::outputRaw80211Packet(IO80211Interface *interface, mbuf_t m)
{
    if (!m) return 0;
    if (fHalDevice) {
        return static_cast<QCAPCIDevice*>(fHalDevice)->transmitRaw80211(m);
    }
    freePacket(m);
    return 0;
}

bool AirportQCA::useAppleRSNSupplicant(IO80211Interface *interface)
{
    /* Delegate 802.1X / WPA2-PSK 4-way handshake to macOS native corewland */
    return true;
}

SInt32 AirportQCA::apple80211Request(
    unsigned int request_type,
    int request_number,
    IO80211Interface *interface,
    void *data)
{
    if (request_type != SIOCGA80211 && request_type != SIOCSA80211) {
        return kIOReturnError;
    }

    switch (request_number) {
        case APPLE80211_IOC_CARD_CAPABILITIES: {
            if (data) {
                auto *cap = (apple80211_capability_data*)data;
                cap->version = 1;
                cap->capabilities[0] = APPLE80211_CAP_11AC | APPLE80211_CAP_11N | APPLE80211_CAP_11G;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_PHY_MODE: {
            if (data && request_type == SIOCGA80211) {
                auto *phy = (apple80211_phymode_data*)data;
                phy->version = 1;
                phy->phymode = APPLE80211_MODE_11AC;
                phy->active_phy_mode = APPLE80211_MODE_11AC;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_STATE: {
            if (data && request_type == SIOCGA80211) {
                auto *state = (apple80211_state_data*)data;
                state->version = 1;
                state->state = fLinkActive ? APPLE80211_S_RUN : APPLE80211_S_INIT;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_RSSI: {
            if (data && request_type == SIOCGA80211) {
                auto *rssi = (apple80211_rssi_data*)data;
                rssi->version = 1;
                rssi->num_rssi = 1;
                rssi->rssi[0] = fRssi;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_NOISE: {
            if (data && request_type == SIOCGA80211) {
                auto *noise = (apple80211_noise_data*)data;
                noise->version = 1;
                noise->num_noise = 1;
                noise->noise[0] = fNoise;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_CHANNEL: {
            if (data) {
                auto *chan = (apple80211_channel_data*)data;
                if (request_type == SIOCGA80211) {
                    chan->version = 1;
                    chan->channel = fChannel;
                    chan->flags = (fChannel > 14) ? APPLE80211_C_FLAG_5GHZ : APPLE80211_C_FLAG_2GHZ;
                } else {
                    fChannel = chan->channel;
                    if (fHalDevice) static_cast<QCAPCIDevice*>(fHalDevice)->setChannel(fChannel);
                }
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_SCAN_REQ: {
            qca_info("apple80211Request: SCAN_REQ");
            if (fHalDevice) {
                static_cast<QCAPCIDevice*>(fHalDevice)->startScan();
            }
            if (fNetIf) {
                fNetIf->postMessage(APPLE80211_M_SCAN_DONE);
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_ASSOCIATE: {
            qca_info("apple80211Request: ASSOCIATE");
            if (data && fHalDevice) {
                auto *assoc = (apple80211_assoc_data*)data;
                static_cast<QCAPCIDevice*>(fHalDevice)->associate(assoc);
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_DISASSOCIATE: {
            qca_info("apple80211Request: DISASSOCIATE");
            if (fHalDevice) {
                static_cast<QCAPCIDevice*>(fHalDevice)->disassociate();
            }
            setLinkStatus(kIONetworkLinkValid);
            fLinkActive = false;
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_CIPHER_KEY: {
            qca_info("apple80211Request: CIPHER_KEY (installing AES/TKIP key)");
            if (data && fHalDevice) {
                auto *key = (apple80211_key*)data;
                static_cast<QCAPCIDevice*>(fHalDevice)->installKey(key);
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_COUNTRY_CODE: {
            if (data && request_type == SIOCSA80211) {
                auto *cc = (apple80211_country_code_data*)data;
                qca_info("apple80211Request: setting country code %c%c", cc->cc[0], cc->cc[1]);
            }
            return kIOReturnSuccess;
        }

        default:
            break;
    }

    return kIOReturnSuccess;
}

const OSString* AirportQCA::newVendorString() const
{
    return OSString::withCString("Qualcomm Atheros");
}

const OSString* AirportQCA::newModelString() const
{
    return OSString::withCString("QCA9377 802.11ac Wireless Network Adapter");
}

bool AirportQCA::createMediumTables(const IONetworkMedium **primary)
{
    OSDictionary *mediumDict = OSDictionary::withCapacity(3);
    if (!mediumDict) return false;

    IONetworkMedium *medium = IONetworkMedium::medium(kIOMediumIEEE80211Auto, 433300000ULL);
    IONetworkMedium::addMedium(mediumDict, medium);
    if (primary) *primary = medium;
    medium->release();

    bool result = publishMediumDictionary(mediumDict);
    mediumDict->release();
    return result;
}

bool AirportQCA::setLinkStatus(UInt32 status, const IONetworkMedium *activeMedium, UInt64 speed, OSData *data)
{
    return super::setLinkStatus(status, activeMedium, speed, data);
}

void AirportQCA::watchdogAction(IOTimerEventSource *timer)
{
    if (fHalDevice && fLinkActive) {
        static_cast<QCAPCIDevice*>(fHalDevice)->watchdogCheck();
    }
    if (fWatchdogTimer) {
        fWatchdogTimer->setTimeoutMS(kWatchDogPeriodMs);
    }
}

bool AirportQCA::initPCIPowerManagement(IOPCIDevice *pci)
{
    PMinit();
    pci->joinPMtree(this);
    registerPowerDriver(this, nullptr, 0);
    return true;
}

IOReturn AirportQCA::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    qca_info("setPowerState: state = %lu", powerStateOrdinal);
    fPowerState = (powerStateOrdinal == 0) ? kQCAPowerStateOff : kQCAPowerStateOn;
    return IOPMAckImplied;
}
