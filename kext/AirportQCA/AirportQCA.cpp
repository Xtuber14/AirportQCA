//
//  AirportQCA.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "AirportQCA.hpp"
#include <IOKit/network/IOOutputQueue.h>
#include "AirportQCAInterface.hpp"
#include "AirportQCAEthernetInterface.hpp"
#include "hw/QCAPCIDevice.hpp"

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportQCA, IO80211Controller)

#define kWatchDogPeriodMs 1000

static IOReturn qcaPrepareBSDInterface(OSObject *owner, void *arg0, void *, void *, void *)
{
    AirportQCA *driver = OSDynamicCast(AirportQCA, owner);
    return driver && driver->getSkywalkInterface()
        ? driver->getSkywalkInterface()->prepareBSDInterface((ifnet_t)arg0, 0)
        : kIOReturnNotReady;
}

bool AirportQCA::init(OSDictionary *properties)
{
    if (!super::init(properties)) {
        return false;
    }

    fPciNub = nullptr;
    fNetIf = nullptr;
    fBSDInterface = nullptr;
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
        stop(provider);
        return false;
    }

    /* Enable Bus Master, Memory, and IO decoding */
    fPciNub->setBusMasterEnable(true);
    fPciNub->setMemoryEnable(true);
    fPciNub->setIOEnable(true);

    if (!createWorkLoop()) {
        qca_err("Failed to create workloop");
        stop(provider);
        return false;
    }

    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        qca_err("Failed to create command gate");
        stop(provider);
        return false;
    }
    fWorkLoop->addEventSource(fCommandGate);

    const IONetworkMedium *primaryMedium = nullptr;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) ||
        !setSelectedMedium(primaryMedium)) {
        qca_err("Failed to setup network mediums");
        stop(provider);
        return false;
    }

    /* Initialize hardware layer */
    auto *hal = new QCAPCIDevice();
    if (!hal || !hal->init(fPciNub, this)) {
        qca_err("Failed to initialize QCAPCIDevice HAL");
        if (hal) delete hal;
        stop(provider);
        return false;
    }
    fHalDevice = hal;

    /* Keep the valid fallback until the HAL can read a provisioned address. */
    uint8_t hardwareAddress[sizeof(fMacAddress.bytes)] = {};
    hal->getMacAddress(hardwareAddress);
    uint8_t macBits = 0;
    for (unsigned i = 0; i < sizeof(hardwareAddress); ++i) macBits |= hardwareAddress[i];
    if (macBits && !(hardwareAddress[0] & 1))
        memcpy(fMacAddress.bytes, hardwareAddress, sizeof(hardwareAddress));

    /* Register the Tahoe Skywalk protocol and its BSD-facing Ethernet interface. */
    fNetIf = new AirportQCAInterface;
    if (!fNetIf || !fNetIf->init()) {
        qca_err("Failed to initialize Tahoe infrastructure interface");
        OSSafeReleaseNULL(fNetIf);
        stop(provider);
        return false;
    }
    fNetIf->setDriver(this);
    fNetIf->setInterfaceRole(1);
    fNetIf->setInterfaceId(1);
    if (!fNetIf->attach(this) || !attachInterface(fNetIf, this) ||
        !IONetworkController::attachInterface((IONetworkInterface **)&fBSDInterface, false)) {
        qca_err("Failed to attach Tahoe Skywalk/BSD interfaces");
        stop(provider);
        return false;
    }
    IOSkywalkEthernetInterface::RegistrationInfo registrationInfo = {};
    if (!fNetIf->initRegistrationInfo(&registrationInfo, 1, sizeof(registrationInfo))) {
        qca_err("Failed to initialize Tahoe Skywalk registration info");
        stop(provider);
        return false;
    }
    fNetIf->mExpansionData->fRegistrationInfo =
        (IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo =
        (IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(IOSkywalkEthernetInterface::RegistrationInfo));
    if (!fNetIf->mExpansionData->fRegistrationInfo || !fNetIf->mExpansionData2->fRegistrationInfo) {
        qca_err("Failed to allocate Tahoe Skywalk registration info");
        stop(provider);
        return false;
    }
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registrationInfo, sizeof(registrationInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registrationInfo, sizeof(registrationInfo));
    fNetIf->deferBSDAttach(true);
    if (!fNetIf->start(this)) {
        qca_err("Failed to start Tahoe Skywalk interface");
        stop(provider);
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
    if (fBSDInterface) fBSDInterface->registerService();

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

    if (fBSDInterface) {
        IONetworkController::detachInterface(fBSDInterface, true);
        fBSDInterface->release();
        fBSDInterface = nullptr;
    }
    if (fNetIf) {
        fNetIf->stop(this);
        if (fNetIf->mExpansionData && fNetIf->mExpansionData->fRegistrationInfo) {
            IOFree(fNetIf->mExpansionData->fRegistrationInfo, sizeof(IOSkywalkNetworkInterface::RegistrationInfo));
            fNetIf->mExpansionData->fRegistrationInfo = nullptr;
        }
        if (fNetIf->mExpansionData2 && fNetIf->mExpansionData2->fRegistrationInfo) {
            IOFree(fNetIf->mExpansionData2->fRegistrationInfo, sizeof(IOSkywalkEthernetInterface::RegistrationInfo));
            fNetIf->mExpansionData2->fRegistrationInfo = nullptr;
        }
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
    AirportQCAEthernetInterface *netif = new AirportQCAEthernetInterface;
    if (!netif || !netif->initWithSkywalkInterface(this, fNetIf)) {
        OSSafeReleaseNULL(netif);
        return nullptr;
    }
    fBSDInterface = netif;
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

#ifdef IO80211FAMILY_V2
IOReturn AirportQCA::setHardwareAddress(const void *addr, UInt32 addrBytes)
{
    if (!addr || addrBytes != sizeof(fMacAddress.bytes)) return kIOReturnBadArgument;
    const uint8_t *bytes = static_cast<const uint8_t *>(addr);
    uint8_t bits = 0;
    for (unsigned i = 0; i < addrBytes; ++i) bits |= bytes[i];
    if (!bits || (bytes[0] & 1)) return kIOReturnBadArgument;
    memcpy(fMacAddress.bytes, bytes, sizeof(fMacAddress.bytes));
    return kIOReturnSuccess;
}
#endif

#ifndef IO80211FAMILY_V2
IOReturn AirportQCA::setHardwareAddress(const IOEthernetAddress *addrP)
{
    if (!addrP) return kIOReturnBadArgument;
    memcpy(fMacAddress.bytes, addrP->bytes, sizeof(fMacAddress.bytes));
    return kIOReturnSuccess;
}

#endif

UInt32 AirportQCA::outputPacket(mbuf_t m, void *param)
{
    if (!m) return 0;
    if (fHalDevice && fLinkActive) {
        UInt32 result = static_cast<QCAPCIDevice*>(fHalDevice)->transmitPacket(m);
        if (result == kIOReturnOutputSuccess) return result;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    freePacket(m);
    return 0;
}

#ifndef IO80211FAMILY_V2
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


SInt32 AirportQCA::dispatchApple80211Request(unsigned int request_type, int request_number, void *data)
{
    if (request_type != SIOCGA80211 && request_type != SIOCSA80211) {
        return kIOReturnError;
    }

    switch (request_number) {
        case APPLE80211_IOC_CARD_CAPABILITIES: {
            if (data) {
                auto *cap = (apple80211_capability_data*)data;
                memset(cap, 0, sizeof(*cap));
                cap->version = APPLE80211_VERSION;
                /* Bit-index capabilities: byte[n] sets bit (cap_index - 8*n) */
                cap->capabilities[0] =
                    (1 << APPLE80211_CAP_WEP) |       /* 0 */
                    (1 << APPLE80211_CAP_TKIP) |      /* 1 */
                    (1 << APPLE80211_CAP_AES_CCM) |   /* 3 */
                    (1 << APPLE80211_CAP_IBSS) |      /* 5 */
                    (1 << APPLE80211_CAP_PMGT);       /* 6 */
                cap->capabilities[1] =
                    (1 << (APPLE80211_CAP_TXPMGT - 8)) |       /* 8 */
                    (1 << (APPLE80211_CAP_SHSLOT - 8)) |       /* 9 */
                    (1 << (APPLE80211_CAP_SHPREAMBLE - 8)) |   /* 10 */
                    (1 << (APPLE80211_CAP_MONITOR - 8)) |      /* 11 */
                    (1 << (APPLE80211_CAP_TKIPMIC - 8)) |      /* 12 */
                    (1 << (APPLE80211_CAP_WPA1 - 8)) |         /* 13 */
                    (1 << (APPLE80211_CAP_WPA2 - 8)) |         /* 14 */
                    (1 << (APPLE80211_CAP_WPA - 8));           /* 15 */
                cap->capabilities[2] =
                    (1 << (APPLE80211_CAP_BURST - 8*2)) |          /* 16 */
                    (1 << (APPLE80211_CAP_WME - 8*2)) |            /* 17 */
                    (1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 8*2)) | /* 18 */
                    (1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 8*2));  /* 19 */
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_PHY_MODE: {
            if (data && request_type == SIOCGA80211) {
                auto *phy = (apple80211_phymode_data*)data;
                phy->version = APPLE80211_VERSION;
                phy->phy_mode = APPLE80211_MODE_11A | APPLE80211_MODE_11B |
                                APPLE80211_MODE_11G | APPLE80211_MODE_11N |
                                APPLE80211_MODE_11AC;
                phy->active_phy_mode = APPLE80211_MODE_11AC;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_STATE: {
            if (data && request_type == SIOCGA80211) {
                auto *state = (apple80211_state_data*)data;
                state->version = APPLE80211_VERSION;
                state->state = fLinkActive ? APPLE80211_S_RUN : APPLE80211_S_INIT;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_RSSI: {
            if (data && request_type == SIOCGA80211) {
                auto *rssi = (apple80211_rssi_data*)data;
                memset(rssi, 0, sizeof(*rssi));
                rssi->version = APPLE80211_VERSION;
                rssi->num_radios = 1;
                rssi->rssi_unit = APPLE80211_UNIT_DBM;
                rssi->rssi[0] = fRssi;
                rssi->aggregate_rssi = fRssi;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_NOISE: {
            if (data && request_type == SIOCGA80211) {
                auto *noise = (apple80211_noise_data*)data;
                memset(noise, 0, sizeof(*noise));
                noise->version = APPLE80211_VERSION;
                noise->num_radios = 1;
                noise->noise_unit = APPLE80211_UNIT_DBM;
                noise->noise[0] = fNoise;
                noise->aggregate_noise = fNoise;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_CHANNEL: {
            if (data) {
                auto *chan = (apple80211_channel_data*)data;
                if (request_type == SIOCGA80211) {
                    chan->version = APPLE80211_VERSION;
                    chan->channel.version = APPLE80211_VERSION;
                    chan->channel.channel = fChannel;
                    chan->channel.flags = APPLE80211_C_FLAG_20MHZ |
                        ((fChannel > 14) ? APPLE80211_C_FLAG_5GHZ : APPLE80211_C_FLAG_2GHZ);
                } else {
                    fChannel = chan->channel.channel;
                    if (fHalDevice) static_cast<QCAPCIDevice*>(fHalDevice)->setChannel(fChannel);
                }
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_OP_MODE: {
            if (data && request_type == SIOCGA80211) {
                auto *op = (apple80211_opmode_data*)data;
                op->version = APPLE80211_VERSION;
                op->op_mode = APPLE80211_M_STA;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_POWER: {
            if (data) {
                auto *pwr = (apple80211_power_data*)data;
                if (request_type == SIOCGA80211) {
                    pwr->version = APPLE80211_VERSION;
                    pwr->num_radios = 1;
                    pwr->power_state[0] = (fPowerState == kQCAPowerStateOn)
                                           ? APPLE80211_POWER_ON : APPLE80211_POWER_OFF;
                }
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_SSID: {
            if (data && request_type == SIOCGA80211) {
                auto *ssid = (apple80211_ssid_data*)data;
                ssid->version = APPLE80211_VERSION;
                ssid->ssid_len = 0; /* populated once associated */
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_BSSID: {
            if (data && request_type == SIOCGA80211) {
                auto *bssid = (apple80211_bssid_data*)data;
                bssid->version = APPLE80211_VERSION;
                memset(&bssid->bssid, 0, sizeof(bssid->bssid));
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_SUPPORTED_CHANNELS: {
            if (data && request_type == SIOCGA80211) {
                auto *sc = (apple80211_sup_channel_data*)data;
                sc->version = APPLE80211_VERSION;
                sc->num_channels = 0;
                /* 2.4 GHz channels 1-11 */
                for (uint32_t i = 1; i <= 11; i++) {
                    sc->supported_channels[sc->num_channels].version = APPLE80211_VERSION;
                    sc->supported_channels[sc->num_channels].channel = i;
                    sc->supported_channels[sc->num_channels].flags =
                        APPLE80211_C_FLAG_2GHZ | APPLE80211_C_FLAG_20MHZ | APPLE80211_C_FLAG_ACTIVE;
                    sc->num_channels++;
                }
                /* 5 GHz channels */
                static const uint32_t chan5g[] = {36,40,44,48, 52,56,60,64, 100,104,108,112,116,
                                                  120,124,128,132,136,140,149,153,157,161,165};
                for (uint32_t j = 0; j < sizeof(chan5g)/sizeof(chan5g[0]); j++) {
                    sc->supported_channels[sc->num_channels].version = APPLE80211_VERSION;
                    sc->supported_channels[sc->num_channels].channel = chan5g[j];
                    sc->supported_channels[sc->num_channels].flags =
                        APPLE80211_C_FLAG_5GHZ | APPLE80211_C_FLAG_20MHZ | APPLE80211_C_FLAG_ACTIVE;
                    sc->num_channels++;
                }
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_LOCALE: {
            if (data && request_type == SIOCGA80211) {
                auto *loc = (apple80211_locale_data*)data;
                loc->version = APPLE80211_VERSION;
                loc->locale = APPLE80211_LOCALE_FCC;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_TXPOWER: {
            if (data && request_type == SIOCGA80211) {
                auto *tp = (apple80211_txpower_data*)data;
                tp->version = APPLE80211_VERSION;
                tp->txpower_unit = APPLE80211_UNIT_DBM;
                tp->txpower = 17; /* 17 dBm typical */
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_POWERSAVE: {
            if (data && request_type == SIOCGA80211) {
                auto *ps = (apple80211_powersave_data*)data;
                ps->version = APPLE80211_VERSION;
                ps->powersave_level = APPLE80211_POWERSAVE_MODE_DISABLED;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_PROTMODE: {
            if (data && request_type == SIOCGA80211) {
                auto *pm = (apple80211_protmode_data*)data;
                pm->version = APPLE80211_VERSION;
                pm->protmode = APPLE80211_PROTMODE_AUTO;
                pm->threshold = 2347;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_AUTH_TYPE: {
            if (data && request_type == SIOCGA80211) {
                auto *auth = (apple80211_authtype_data*)data;
                auth->version = APPLE80211_VERSION;
                auth->authtype_lower = APPLE80211_AUTHTYPE_OPEN;
                auth->authtype_upper = APPLE80211_AUTHTYPE_NONE;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_ASSOCIATE_RESULT: {
            if (data && request_type == SIOCGA80211) {
                auto *ar = (apple80211_assoc_result_data*)data;
                ar->version = APPLE80211_VERSION;
                ar->result = fLinkActive ? APPLE80211_RESULT_SUCCESS : APPLE80211_RESULT_UNAVAILABLE;
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_DRIVER_VERSION: {
            if (data && request_type == SIOCGA80211) {
                auto *dv = (apple80211_version_data*)data;
                dv->version = APPLE80211_VERSION;
                dv->string_len = (u_int16_t)snprintf(dv->string, sizeof(dv->string),
                                          "AirportQCA 1.0.0 (QCA9377)");
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_HARDWARE_VERSION: {
            if (data && request_type == SIOCGA80211) {
                auto *hv = (apple80211_version_data*)data;
                hv->version = APPLE80211_VERSION;
                hv->string_len = (u_int16_t)snprintf(hv->string, sizeof(hv->string),
                                          "Qualcomm Atheros QCA9377 Rev 1.0");
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_RATE: {
            if (data && request_type == SIOCGA80211) {
                auto *rate = (apple80211_rate_data*)data;
                rate->version = APPLE80211_VERSION;
                rate->num_radios = 1;
                rate->rate[0] = 433; /* 433 Mbps MCS9 VHT80 1SS */
            }
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_MCS_INDEX_SET:
        case APPLE80211_IOC_ROAM_THRESH:
        case APPLE80211_IOC_INT_MIT: {
            return kIOReturnSuccess;
        }

        case APPLE80211_IOC_SCAN_REQ: {
            qca_info("apple80211Request: SCAN_REQ");
            if (fHalDevice) {
                static_cast<QCAPCIDevice*>(fHalDevice)->startScan();
            }
            if (fNetIf) {
                fNetIf->postMessage(APPLE80211_M_SCAN_DONE, nullptr, 0, false);
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

#endif


#ifndef IO80211FAMILY_V2
SInt32 AirportQCA::apple80211Request(unsigned int type, int number, IO80211Interface *, void *data)
{
    return dispatchApple80211Request(type, number, data);
}
#endif


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
    bool result = super::setLinkStatus(status, activeMedium, speed, data);
    if (fNetIf) {
        bool active = (status & kIONetworkLinkActive) != 0;
        fNetIf->setLinkState(active ? kIO80211NetworkLinkUp : kIO80211NetworkLinkDown, 0, false, 30, 0);
        if (fBSDInterface) fBSDInterface->setLinkState(active ? kIO80211NetworkLinkUp : kIO80211NetworkLinkDown);
    }
    return result;
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

void AirportQCA::interfaceSetChannel(UInt32 channel)
{
    if (channel == 0 || channel > 196) return;
    fChannel = channel;
    if (fHalDevice) static_cast<QCAPCIDevice *>(fHalDevice)->setChannel(channel);
}

void AirportQCA::interfaceSetPower(bool enabled)
{
    if (fBSDInterface) {
        if (enabled) enable(fBSDInterface);
        else disable(fBSDInterface);
    }
}

IOReturn AirportQCA::prepareSkywalkBSDInterface(ifnet_t interface)
{
    return fCommandGate && interface
        ? fCommandGate->runAction(qcaPrepareBSDInterface, interface)
        : kIOReturnNotReady;
}

#ifdef IO80211FAMILY_V2
bool AirportQCA::isCommandProhibited(int) { return false; }
SInt32 AirportQCA::handleCardSpecific(IO80211SkywalkInterface *, unsigned long, void *, bool) { return kIOReturnUnsupported; }
IOReturn AirportQCA::getDRIVER_VERSION(IO80211SkywalkInterface *, apple80211_version_data *d) { if (!d) return kIOReturnBadArgument; memset(d, 0, sizeof(*d)); d->version = APPLE80211_VERSION; d->string_len = (u_int16_t)snprintf(d->string, sizeof(d->string), "AirportQCA Tahoe"); return kIOReturnSuccess; }
IOReturn AirportQCA::getHARDWARE_VERSION(IO80211SkywalkInterface *, apple80211_version_data *d) { if (!d) return kIOReturnBadArgument; memset(d, 0, sizeof(*d)); d->version = APPLE80211_VERSION; d->string_len = (u_int16_t)snprintf(d->string, sizeof(d->string), "Qualcomm Atheros QCA9377"); return kIOReturnSuccess; }
IOReturn AirportQCA::getCARD_CAPABILITIES(IO80211SkywalkInterface *, apple80211_capability_data *d)
{
    if (!d) return kIOReturnBadArgument;
    memset(d, 0, sizeof(*d));
    d->version = APPLE80211_VERSION;
    d->capabilities[0] = (1 << APPLE80211_CAP_WEP) | (1 << APPLE80211_CAP_TKIP) |
        (1 << APPLE80211_CAP_AES_CCM) | (1 << APPLE80211_CAP_IBSS) | (1 << APPLE80211_CAP_PMGT);
    d->capabilities[1] = (1 << (APPLE80211_CAP_TXPMGT - 8)) |
        (1 << (APPLE80211_CAP_SHSLOT - 8)) | (1 << (APPLE80211_CAP_SHPREAMBLE - 8)) |
        (1 << (APPLE80211_CAP_MONITOR - 8)) | (1 << (APPLE80211_CAP_TKIPMIC - 8)) |
        (1 << (APPLE80211_CAP_WPA1 - 8)) | (1 << (APPLE80211_CAP_WPA2 - 8)) |
        (1 << (APPLE80211_CAP_WPA - 8));
    d->capabilities[2] = (1 << (APPLE80211_CAP_BURST - 16)) |
        (1 << (APPLE80211_CAP_WME - 16)) | (1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 16));
    return kIOReturnSuccess;
}
IOReturn AirportQCA::getPOWER(IO80211SkywalkInterface *, apple80211_power_data *d) { if (!d) return kIOReturnBadArgument; memset(d, 0, sizeof(*d)); d->version = APPLE80211_VERSION; d->num_radios = 1; d->power_state[0] = fPowerState == kQCAPowerStateOn ? APPLE80211_POWER_ON : APPLE80211_POWER_OFF; return kIOReturnSuccess; }
IOReturn AirportQCA::setPOWER(IO80211SkywalkInterface *, apple80211_power_data *d) { if (!d) return kIOReturnBadArgument; interfaceSetPower(d->power_state[0] != APPLE80211_POWER_OFF); return kIOReturnSuccess; }
IOReturn AirportQCA::getCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *d) { if (!d) return kIOReturnBadArgument; memset(d, 0, sizeof(*d)); d->version = APPLE80211_VERSION; return kIOReturnSuccess; }
IOReturn AirportQCA::setCOUNTRY_CODE(IO80211SkywalkInterface *, apple80211_country_code_data *) { return kIOReturnSuccess; }
IOReturn AirportQCA::setGET_DEBUG_INFO(IO80211SkywalkInterface *, apple80211_debug_command *) { return kIOReturnUnsupported; }
CCLogStream *AirportQCA::getLogger() const { return nullptr; }
CommonFaultReporter *AirportQCA::getFaultReporterFromDriver() { return nullptr; }
UInt32 AirportQCA::hardwareOutputQueueDepth() { return 64; }
SInt32 AirportQCA::performCountryCodeOperation(IO80211CountryCodeOp) { return kIOReturnSuccess; }
SInt32 AirportQCA::monitorModeSetEnabled(bool, unsigned int) { return kIOReturnUnsupported; }
#else
// -------------------------------------------------------
// IO80211Controller pure virtual overrides
// -------------------------------------------------------

SInt32 AirportQCA::stopDMA()
{
    qca_info("stopDMA");
    if (fHalDevice) {
        static_cast<QCAPCIDevice*>(fHalDevice)->stop();
    }
    return kIOReturnSuccess;
}

UInt32 AirportQCA::hardwareOutputQueueDepth(IO80211Interface *interface)
{
    return 0; /* No internal queue — packets forwarded directly to CE pipes */
}

SInt32 AirportQCA::performCountryCodeOperation(IO80211Interface *interface, IO80211CountryCodeOp op)
{
    qca_info("performCountryCodeOperation: op=%d", (int)op);
    return kIOReturnSuccess;
}

SInt32 AirportQCA::enableFeature(IO80211FeatureCode code, void *data)
{
    qca_info("enableFeature: code=%d", (int)code);
    return kIOReturnSuccess;
}

// -------------------------------------------------------
// IO80211Controller virtual overrides (concrete stubs)
// -------------------------------------------------------

int AirportQCA::bpfOutputPacket(OSObject *obj, UInt type, mbuf_t m)
{
    if (m) freePacket(m);
    return 0;
}

IOReturn AirportQCA::getHardwareAddressForInterface(IO80211Interface *interface, IOEthernetAddress *addr)
{
    return getHardwareAddress(addr);
}

SInt32 AirportQCA::monitorModeSetEnabled(IO80211Interface *interface, bool enabled, UInt channel)
{
    qca_info("monitorModeSetEnabled: enabled=%d ch=%u", enabled, channel);
    return kIOReturnSuccess;
}

void AirportQCA::requestPacketTx(void *data, UInt length)
{
    /* Intentionally empty — packet Tx driven by outputPacket */
}

SInt32 AirportQCA::enableVirtualInterface(IO80211VirtualInterface *vif)
{
    return kIOReturnUnsupported;
}

SInt32 AirportQCA::disableVirtualInterface(IO80211VirtualInterface *vif)
{
    return kIOReturnUnsupported;
}

IO80211VirtualInterface* AirportQCA::createVirtualInterface(ether_addr *addr, UInt role)
{
    return nullptr; /* P2P / AWDL virtual interfaces not supported */
}

SInt32 AirportQCA::apple80211VirtualRequest(UInt request_type, int request_number, IO80211VirtualInterface *vif, void *data)
{
    return kIOReturnUnsupported;
}

#endif
