//
//  AirportQCA.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "AirportQCA.hpp"
#include "QCAPCIProvider.hpp"
#include <IOKit/network/IOOutputQueue.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include "AirportQCAInterface.hpp"
#include "AirportQCAEthernetInterface.hpp"
#include "hw/QCAPCIDevice.hpp"
#include "WCL/WCL.hpp"

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

static void qcaSetStartStage(IORegistryEntry *entry, const char *stage)
{
    /* Start-stage breadcrumb on the persistent provider node: driver
       IOLog output only reaches the boot console, never the unified
       log, so after a failed boot the last QCAStartStage visible in
       ioreg marks the exact step that failed. */
    if (entry) {
        entry->setProperty("QCAStartStage", stage);
    }
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
    fWorkQueue = nullptr;
    fCommandGate = nullptr;
    fInterruptSource = nullptr;
    fWatchdogTimer = nullptr;
    fHalDevice = nullptr;

    fPowerState = kQCAPowerStateOff;
    fLinkActive = false;
    fChannel = 1;
    fRssi = -50;
    fNoise = -95;

#ifdef IO80211FAMILY_V2
    fDriverLogPipe = nullptr;
    fDriverDataPathPipe = nullptr;
    fDriverSnapshotsPipe = nullptr;
    fDriverLogger = nullptr;
    fDriverFaultStream = nullptr;
    fDriverCoreFaultReporter = nullptr;
    fDriverFaultReporter = nullptr;
#endif

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
#ifdef IO80211FAMILY_V2
    releaseCCLogs();
#endif
    if (fHalDevice) {
        delete static_cast<QCAPCIDevice*>(fHalDevice);
        fHalDevice = nullptr;
    }
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = nullptr;
    }
    if (fWorkQueue) {
        fWorkQueue->release();
        fWorkQueue = nullptr;
    }
    super::free();
}

IOService* AirportQCA::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score)) {
        return nullptr;
    }

    QCAPCIProvider *pciProvider = OSDynamicCast(QCAPCIProvider, provider);
    if (!pciProvider || !pciProvider->pciNub) {
        return nullptr;
    }

    UInt16 vendorId = pciProvider->pciNub->configRead16(kIOPCIConfigVendorID);
    UInt16 deviceId = pciProvider->pciNub->configRead16(kIOPCIConfigDeviceID);

    if (vendorId == QCA9377_PCI_VENDOR_ID && deviceId == QCA9377_1_0_DEVICE_ID) {
        qca_info("probe: matched Qualcomm Atheros QCA9377 (%04x:%04x)", vendorId, deviceId);
        pciProvider->setProperty("QCAStartStage", "controller-matched");
        *score += 2000;
        return this;
    }

    return nullptr;
}

bool AirportQCA::createWorkLoop()
{
    if (!fWorkLoop) {
        if (fWorkQueue) {
            /* Alias the shared WCL work queue as the controller work loop;
               the extra retain keeps stop()/free()'s two releases balanced. */
            fWorkQueue->retain();
            fWorkLoop = fWorkQueue;
        } else {
            fWorkLoop = IOWorkLoop::workLoop();
        }
    }
    return (fWorkLoop != nullptr);
}

#ifdef IO80211FAMILY_V2
bool AirportQCA::createWorkQueue()
{
    /* The QCAPCIProvider creates the shared IO80211WorkQueue at its early
       start and AirportQCA::start shares it; only create one here if the
       provider's queue was not available, so the single release in
       stop()/free() stays balanced. It must never replace fWorkLoop
       directly — createWorkLoop() aliases it with a retain. */
    if (!fWorkQueue) {
        fWorkQueue = IO80211WorkQueue::workQueue();
    }
    return (fWorkQueue != nullptr);
}

IO80211WorkQueue* AirportQCA::getWorkQueue() const
{
    return fWorkQueue;
}
#endif

IOWorkLoop* AirportQCA::getWorkLoop() const
{
    return fWorkLoop;
}

bool AirportQCA::start(IOService *provider)
{
    /* The controller personality is gated on IOResourceMatch=IOBSD and
       matches the QCAPCIProvider instance, the way AirportItlwm matches
       its IOPCIEDeviceWrapper. The provider owns the PCI nub and the
       IO80211WorkQueue created at its early start. */
    QCAPCIProvider *pciProvider = OSDynamicCast(QCAPCIProvider, provider);
    if (!pciProvider || !pciProvider->pciNub) {
        qca_err("start: provider is not a QCAPCIProvider with a PCI nub");
        return false;
    }
    fPciNub = pciProvider->pciNub;
    qcaSetStartStage(pciProvider, "start-entered");

#ifdef IO80211FAMILY_V2
    /* Share the provider's IO80211WorkQueue: it becomes the controller
       work loop and runs the WCL fault reporter. Each pointer carries
       exactly one retain (share + alias), released once in stop()/free(). */
    if (!fWorkQueue && pciProvider->workQueue) {
        fWorkQueue = pciProvider->workQueue;
        fWorkQueue->retain();
    }
    if (!createWorkLoop()) {
        qca_err("Failed to create workloop");
        return false;
    }

    qcaSetStartStage(pciProvider, "wcl-init");
    /* Full WCL init runs at this late stage — the IOResourceMatch=IOBSD
       gate means the WCL/Skywalk environment exists here, the same stage
       AirportItlwm runs its initCCLogs(). The parent's createIOReporters
       consumes getLogger()/getLogPipes() during IO80211Controller::start,
       so everything must be ready before super::start. */
    if (!initCCLogs()) {
        releaseCCLogs();
        return false;
    }
#endif

    qcaSetStartStage(pciProvider, "super-start");
    if (!super::start(provider)) {
#ifdef IO80211FAMILY_V2
        releaseCCLogs();
#endif
        return false;
    }

    /* Enable Bus Master, Memory, and IO decoding */
    fPciNub->setBusMasterEnable(true);
    fPciNub->setMemoryEnable(true);
    fPciNub->setIOEnable(true);
    fPciNub->configWrite8(0x41, 0);

    /* Bring the device out of any low-power state before touching the
       hardware and register device power management now — the same
       sequence AirportItlwm runs before its HAL attach. Failures are
       logged but non-fatal so the stage breadcrumb still records how
       far the hardware bring-up gets. */
    qcaSetStartStage(pciProvider, "power-domain");
    IOPowerConnection *powerParent = (IOPowerConnection *) getParentEntry(gIOPowerPlane);
    if (powerParent && fPciNub->requestPowerDomainState(
            kIOPMPowerOn, powerParent, IOPMLowestState) != IOPMNoErr) {
        qca_err("requestPowerDomainState failed");
    }
    qcaSetStartStage(pciProvider, "pm");
    initPCIPowerManagement(fPciNub);

    qcaSetStartStage(pciProvider, "command-gate");
    fCommandGate = IOCommandGate::commandGate(this);
    if (!fCommandGate) {
        qca_err("Failed to create command gate");
        stop(provider);
        return false;
    }
    fWorkLoop->addEventSource(fCommandGate);

    qcaSetStartStage(pciProvider, "mediums");
    const IONetworkMedium *primaryMedium = nullptr;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) ||
        !setSelectedMedium(primaryMedium)) {
        qca_err("Failed to setup network mediums");
        stop(provider);
        return false;
    }

    qcaSetStartStage(pciProvider, "hal-init");
    /* Initialize hardware layer */
    auto *hal = new QCAPCIDevice();
    if (!hal || !hal->init(fPciNub, this)) {
        qca_err("Failed to initialize QCAPCIDevice HAL");
        if (hal) delete hal;
        stop(provider);
        return false;
    }
    fHalDevice = hal;
    qcaSetStartStage(pciProvider, "hal-done");

    /* Keep the valid fallback until the HAL can read a provisioned address. */
    uint8_t hardwareAddress[sizeof(fMacAddress.bytes)] = {};
    hal->getMacAddress(hardwareAddress);
    uint8_t macBits = 0;
    for (unsigned i = 0; i < sizeof(hardwareAddress); ++i) macBits |= hardwareAddress[i];
    if (macBits && !(hardwareAddress[0] & 1))
        memcpy(fMacAddress.bytes, hardwareAddress, sizeof(hardwareAddress));

    qcaSetStartStage(pciProvider, "netif-init");
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
    qcaSetStartStage(pciProvider, "netif-start");
    if (!fNetIf->start(this)) {
        qca_err("Failed to start Tahoe Skywalk interface");
        stop(provider);
        return false;
    }

    qcaSetStartStage(pciProvider, "watchdog");
    /* Setup Watchdog Timer */
    fWatchdogTimer = IOTimerEventSource::timerEventSource(
        this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportQCA::watchdogAction)
    );
    if (fWatchdogTimer) {
        fWorkLoop->addEventSource(fWatchdogTimer);
        fWatchdogTimer->setTimeoutMS(kWatchDogPeriodMs);
    }

    setLinkStatus(kIONetworkLinkValid);
    registerService();
    if (fBSDInterface) fBSDInterface->registerService();
    qcaSetStartStage(pciProvider, "registered");

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

#ifdef IO80211FAMILY_V2
    releaseCCLogs();
#endif

    /* Each pointer carries one retain: fWorkLoop aliases the shared queue
       (retain in createWorkLoop) and fWorkQueue is the shared queue
       (retain in start). Release both. */
    if (fWorkLoop) {
        fWorkLoop->release();
        fWorkLoop = nullptr;
    }
    if (fWorkQueue) {
        fWorkQueue->release();
        fWorkQueue = nullptr;
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
    /* Device enable only means firmware is running. The link must stay down
     * until the station association path confirms a peer is actually joined. */
    setLinkStatus(kIONetworkLinkValid);
    fLinkActive = false;
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
#endif

SInt32 AirportQCA::dispatchApple80211Request(unsigned int request_type, int request_number,
                                             void *data, UInt32 requestedChannel)
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
            if (!fHalDevice) return kIOReturnNotReady;
            return static_cast<QCAPCIDevice*>(fHalDevice)->startScan() == 0
                ? kIOReturnSuccess : kIOReturnIOError;
        }

        case APPLE80211_IOC_ASSOCIATE: {
            qca_info("apple80211Request: ASSOCIATE");
            if (!data || !fHalDevice) return kIOReturnNotReady;
            return static_cast<QCAPCIDevice*>(fHalDevice)->associate(
                (apple80211_assoc_data*)data, requestedChannel ? requestedChannel : fChannel) == 0
                ? kIOReturnSuccess : kIOReturnIOError;
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
    if (fHalDevice) {
        static_cast<QCAPCIDevice*>(fHalDevice)->watchdogCheck();
    }
    if (fWatchdogTimer) {
        fWatchdogTimer->setTimeoutMS(kWatchDogPeriodMs);
    }
}

void AirportQCA::notifyScanComplete()
{
    if (fNetIf) fNetIf->postMessage(APPLE80211_M_SCAN_DONE, nullptr, 0, false);
}

void AirportQCA::reportScanBeacon(const u8 *frame, u32 frameLength, u32 channel, u32 snr)
{
    static const u32 kBeaconHeaderLength = 24 + 12;
    if (!fNetIf || !frame || frameLength < kBeaconHeaderLength ||
        frameLength > 2048 || channel < 1 || channel > 196 ||
        (channel > 14 && channel < 36))
        return;

    const u16 frameControl = (u16)((u16)frame[0] | ((u16)frame[1] << 8));
    const u16 subtype = frameControl & 0x00f0;
    if ((frameControl & 0x000c) != 0 || (subtype != 0x0080 && subtype != 0x0050))
        return;

    const u8 *bssid = frame + 16;
    if ((bssid[0] & 1) != 0) return;
    const u8 *ies = frame + kBeaconHeaderLength;
    const u32 ieLength = frameLength - kBeaconHeaderLength;
    if (!AirportWCL::validIEs(ies, ieLength)) return;

    AirportWCL::BeaconMetadata *metadata = static_cast<AirportWCL::BeaconMetadata *>(
        IOMalloc(sizeof(AirportWCL::BeaconMetadata) + ieLength));
    if (!metadata) return;
    *metadata = {};
    metadata->ieLength = ieLength;
    metadata->channel = (u8)channel;
    metadata->channelSpec = (u16)(channel | (channel > 14 ? 0xd000 : 0x1000));
    memcpy(metadata->bssid, bssid, sizeof(metadata->bssid));
    metadata->interval = (u16)((u16)frame[32] | ((u16)frame[33] << 8));
    metadata->capability = (u16)((u16)frame[34] | ((u16)frame[35] << 8));
    metadata->noise = -95;
    metadata->snr = (int16_t)(snr > 255 ? 255 : snr);
    metadata->rssi = metadata->snr + metadata->noise;
    if (metadata->rssi > 0) metadata->rssi = 0;
    metadata->flags = AirportWCL::RSSIValid;

    for (u32 offset = 0; offset + 2 <= ieLength; ) {
        const u32 size = (u32)ies[offset + 1] + 2;
        if (size > ieLength - offset) break;
        if (ies[offset] == 0) {
            metadata->ssidLength = (u8)(ies[offset + 1] > sizeof(metadata->ssid)
                ? sizeof(metadata->ssid) : ies[offset + 1]);
            memcpy(metadata->ssid, ies + offset + 2, metadata->ssidLength);
            metadata->flags |= AirportWCL::SSIDPresent;
            break;
        }
        offset += size;
    }

    memcpy(metadata + 1, ies, ieLength);
    metadata->ieLength = (u32)AirportWCL::selectWPA2TransitionMode(
        reinterpret_cast<u8 *>(metadata + 1), ieLength);
    const size_t messageLength = sizeof(*metadata) + metadata->ieLength;
    postMessage(fNetIf, AirportWCL::ScanResult, metadata, messageLength, true);
    IOFree(metadata, sizeof(*metadata) + ieLength);
}

bool AirportQCA::initPCIPowerManagement(IOPCIDevice *pci)
{
    /* Mirror AirportItlwm::initPCIPowerManagment: rewrite the command
       register (BusMaster | Memory | MemWrInvalidate, IO space off) and
       force the device to D0 through the PCI Power Management
       capability, settling 10 ms before the first MMIO access. Without
       the PMCSR write the QCA9377 stays in the BIOS ACPI power state
       and every target register read returns 0xFFFFFFFF. */
    UInt16 reg16 = pci->configRead16(kIOPCIConfigCommand);
    reg16 |= (kIOPCICommandBusMaster |
              kIOPCICommandMemorySpace |
              kIOPCICommandMemWrInvalidate);
    reg16 &= ~kIOPCICommandIOSpace;
    pci->configWrite16(kIOPCIConfigCommand, reg16);

    UInt8 pmPCICapPtr = 0;
    pci->findPCICapability(kIOPCIPowerManagementCapability, &pmPCICapPtr);
    if (pmPCICapPtr) {
        pci->configWrite16(pmPCICapPtr + 4, 0x8000);
        IOSleep(10);
    } else {
        qca_info("No PCI Power Management capability; device assumed D0");
    }

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
bool AirportQCA::initCCLogs()
{
    if (fDriverLogger) {
        return true;
    }

    /* Runs at the controller's late start stage: the IOResourceMatch=IOBSD
       gate on the AirportQCA personality keeps this out of the early boot
       stage, so the WCL/Skywalk environment exists here — the same stage
       AirportItlwm runs its initCCLogs(). The parent's createIOReporters
       consumes getLogger()/getLogPipes() during IO80211Controller::start,
       so everything must be ready before super::start. */
    if (!createWorkQueue() || !getWorkLoop()) {
        qca_err("WCL work queue unavailable");
        return false;
    }

    CCPipeOptions logOptions = {};
    logOptions.pipe_type = 0;
    logOptions.log_data_type = 1;
    logOptions.pipe_size = 0x200000;
    logOptions.min_log_size_notify = 0xccccc;
    logOptions.notify_threshold = 1000;
    strlcpy(logOptions.file_name, "AirportQCA_Logs", sizeof(logOptions.file_name));
    snprintf(logOptions.name, sizeof(logOptions.name), "wlan0");
    strlcpy(logOptions.directory_name, "WiFi", sizeof(logOptions.directory_name));
    logOptions.pad9 = 0x1000000;
    logOptions.pad10 = 2;
    fDriverLogPipe = CCPipe::withOwnerNameCapacity(this,
        "com.ath10k.AirportQCA", "DriverLogs", &logOptions);
    if (!fDriverLogPipe || !fDriverLogPipe->CCPipe::startPipe()) {
        qca_err("DriverLogs pipe setup failed");
        return false;
    }

    CCStreamOptions loggerOptions = {};
    CCStream *logger = CCStream::withPipeAndName(fDriverLogPipe,
        "AirportQCA", &loggerOptions);
    fDriverLogger = OSDynamicCast(CCLogStream, logger);
    if (!fDriverLogger) {
        OSSafeReleaseNULL(logger);
        return false;
    }

    memset(&logOptions, 0, sizeof(logOptions));
    logOptions.pipe_type = 0;
    logOptions.log_data_type = 0;
    logOptions.pipe_size = 0x200000;
    logOptions.min_log_size_notify = 0xccccc;
    logOptions.notify_threshold = 1000;
    strlcpy(logOptions.file_name, "AirportQCA_Datapath",
            sizeof(logOptions.file_name));
    strlcpy(logOptions.directory_name, "WiFi", sizeof(logOptions.directory_name));
    logOptions.pad9 = (uint32_t)(0x202800000 >> 32);
    logOptions.pad10 = (uint32_t)0x202800000;
    fDriverDataPathPipe = CCPipe::withOwnerNameCapacity(this,
        "com.ath10k.AirportQCA", "DatapathEvents", &logOptions);
    if (!fDriverDataPathPipe || !fDriverDataPathPipe->CCPipe::startPipe()) {
        qca_err("DatapathEvents pipe setup failed");
        return false;
    }

    memset(&logOptions, 0, sizeof(logOptions));
    logOptions.pipe_type = 0x200000001;
    logOptions.log_data_type = 2;
    logOptions.pipe_size = 128;
    strlcpy(logOptions.file_name, "StateSnapshots", sizeof(logOptions.file_name));
    strlcpy(logOptions.name, "0", sizeof(logOptions.name));
    strlcpy(logOptions.directory_name, "WiFi", sizeof(logOptions.directory_name));
    fDriverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this,
        "com.ath10k.AirportQCA", "StateSnapshots", &logOptions);
    if (!fDriverSnapshotsPipe || !fDriverSnapshotsPipe->CCPipe::startPipe()) {
        qca_err("StateSnapshots pipe setup failed");
        return false;
    }

    CCStreamOptions faultOptions = {};
    faultOptions.stream_type = 1;
    faultOptions.console_level = 0xFFFFFFFFFFFFFFFF;
    fDriverFaultStream = CCStream::withPipeAndName(fDriverSnapshotsPipe,
        "FaultReporter", &faultOptions);
    CCDataStream *dataStream = OSDynamicCast(CCDataStream, fDriverFaultStream);
    if (!dataStream) {
        qca_err("FaultReporter stream creation failed");
        return false;
    }

    fDriverCoreFaultReporter = CCFaultReporter::withStreamWorkloop(
        dataStream, fWorkQueue);
    if (!fDriverCoreFaultReporter) {
        qca_err("CCFaultReporter creation failed");
        return false;
    }

    fDriverFaultReporter = IO80211FaultReporter::allocWithParams(
        fDriverCoreFaultReporter);
    if (!OSDynamicCast(CommonFaultReporter, fDriverFaultReporter)) {
        qca_err("IO80211FaultReporter creation failed");
        return false;
    }

    return fDriverLogPipe && fDriverDataPathPipe && fDriverSnapshotsPipe &&
           fDriverLogger && fDriverFaultReporter;
}

void AirportQCA::releaseCCLogs()
{
    if (fDriverSnapshotsPipe && fDriverSnapshotsPipe->getWorkLoop()) {
        fDriverSnapshotsPipe->CCPipe::stopPipe();
    }
    if (fDriverDataPathPipe && fDriverDataPathPipe->getWorkLoop()) {
        fDriverDataPathPipe->CCPipe::stopPipe();
    }
    if (fDriverLogPipe && fDriverLogPipe->getWorkLoop()) {
        fDriverLogPipe->CCPipe::stopPipe();
    }
    OSSafeReleaseNULL(fDriverFaultReporter);
    OSSafeReleaseNULL(fDriverCoreFaultReporter);
    OSSafeReleaseNULL(fDriverFaultStream);
    OSSafeReleaseNULL(fDriverLogger);
    OSSafeReleaseNULL(fDriverLogPipe);
    OSSafeReleaseNULL(fDriverDataPathPipe);
    OSSafeReleaseNULL(fDriverSnapshotsPipe);
}
#endif

#ifdef IO80211FAMILY_V2
bool AirportQCA::isCommandProhibited(int) { return false; }
SInt32 AirportQCA::enableFeature(IO80211FeatureCode code, void *data)
{
    qca_info("enableFeature: code=%d", (int)code);
    return kIOReturnSuccess;
}
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
CCLogStream *AirportQCA::getLogger() const { return fDriverLogger; }
CommonFaultReporter *AirportQCA::getFaultReporterFromDriver() { return fDriverFaultReporter; }

bool AirportQCA::getLogPipes(CCPipe **logPipe, CCPipe **eventPipe,
                             CCPipe **snapshotsPipe)
{
    bool found = false;
    if (logPipe) {
        *logPipe = fDriverLogPipe;
        found = true;
    }
    if (eventPipe) {
        *eventPipe = fDriverDataPathPipe;
        found = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = fDriverSnapshotsPipe;
        found = true;
    }
    return found;
}
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
