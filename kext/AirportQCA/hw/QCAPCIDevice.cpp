//
//  QCAPCIDevice.cpp
//  AirportQCA - Qualcomm Atheros QCA9377 Native macOS Driver
//

#include "QCAPCIDevice.hpp"
#include "QCACopyEngine.hpp"
#include "QCABMI.hpp"
#include "QCAHTC.hpp"
#include "QCAWMI.hpp"
#include "QCAHTT.hpp"
#include "../AirportQCA.hpp"

QCAPCIDevice::QCAPCIDevice()
    : fPciNub(nullptr),
      fDriver(nullptr),
      fMmioMap(nullptr),
      fMmioBase(nullptr),
      fCopyEngine(nullptr),
      fBmi(nullptr),
      fHtc(nullptr),
      fWmi(nullptr),
      fHtt(nullptr),
      fFwRunning(false)
{
    memset(fMacAddr, 0, sizeof(fMacAddr));
}

QCAPCIDevice::~QCAPCIDevice()
{
    stop();
    if (fHtt) { delete fHtt; fHtt = nullptr; }
    if (fWmi) { delete fWmi; fWmi = nullptr; }
    if (fHtc) { delete fHtc; fHtc = nullptr; }
    if (fBmi) { delete fBmi; fBmi = nullptr; }
    if (fCopyEngine) { delete fCopyEngine; fCopyEngine = nullptr; }

    if (fMmioMap) {
        fMmioMap->release();
        fMmioMap = nullptr;
        fMmioBase = nullptr;
    }
}

bool QCAPCIDevice::init(IOPCIDevice *pciNub, AirportQCA *driver)
{
    if (!pciNub || !driver) return false;

    fPciNub = pciNub;
    fDriver = driver;

    /* Map BAR 0 MMIO */
    fMmioMap = fPciNub->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fMmioMap) {
        qca_err("Failed to map BAR0 MMIO space");
        return false;
    }
    fMmioBase = (volatile void*)fMmioMap->getVirtualAddress();
    qca_info("BAR0 MMIO mapped at virtual address %p (size %llu)",
             fMmioBase, fMmioMap->getLength());

    /* Initialize Copy Engine */
    fCopyEngine = new QCACopyEngine(this);
    if (!fCopyEngine || !fCopyEngine->initPipes()) {
        qca_err("Failed to initialize Copy Engine");
        return false;
    }

    fBmi = new QCABMI(this);
    fHtc = new QCAHTC(this);
    fWmi = new QCAWMI(this);
    fHtt = new QCAHTT(this);

    /* Reset and boot target firmware */
    if (!bootFirmware()) {
        qca_err("Firmware boot sequence failed");
        return false;
    }

    return true;
}

int QCAPCIDevice::warmReset()
{
    qca_info("Executing PCIe warm reset");
    u32 val = read32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS);
    val &= ~CORE_CTRL_PCIE_REG_31_MASK;
    write32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS, val);
    qca_mdelay(10);
    val |= CORE_CTRL_PCIE_REG_31_MASK;
    write32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS, val);
    qca_mdelay(10);
    return 0;
}

int QCAPCIDevice::chipReset()
{
    qca_info("Executing SoC global chip reset");
    u32 val = read32(SOC_GLOBAL_RESET_ADDRESS);
    val |= 1;
    write32(SOC_GLOBAL_RESET_ADDRESS, val);
    qca_mdelay(20);
    return 0;
}

int QCAPCIDevice::waitForTargetInit()
{
    qca_info("Waiting for target firmware to initialize");
    int timeoutMs = 1500;
    while (timeoutMs > 0) {
        u32 val = read32(FW_INDICATOR_ADDRESS);
        if (val == 0xFFFFFFFF) {
            qca_mdelay(10);
            timeoutMs -= 10;
            continue;
        }
        if (val & FW_IND_EVENT_PENDING) {
            qca_err("Target reported firmware crash during init (0x%08x)", val);
            return -1;
        }
        if (val & FW_IND_INITIALIZED) {
            qca_info("Target initialized successfully (FW_INDICATOR = 0x%08x)", val);
            return 0;
        }
        qca_mdelay(10);
        timeoutMs -= 10;
    }
    qca_err("Timed out waiting for target initialization");
    return -1;
}

bool QCAPCIDevice::bootFirmware()
{
    /* 1. Reset chip */
    chipReset();

    /* 2. Wait for ROM bootloader */
    if (waitForTargetInit() != 0) {
        qca_err("Target ROM bootloader failed to respond");
        return false;
    }

    /* 3. Query Target Info via BMI */
    struct bmi_target_info info;
    if (fBmi->getTargetInfo(&info) == 0) {
        qca_info("Target Xtensa CPU active: version 0x%08x", info.version);
    }

    /* 4. Boost clock PLL to 176 MHz */
    fBmi->writeSocRegister(RTC_SOC_BASE_ADDRESS + RTC_POWER_REG_ADDRESS, RTC_POWER_REG_V_ON);

    /* 5. Complete BMI Phase */
    fBmi->done();

    /* 6. Wait for firmware RTOS ready */
    if (waitForTargetInit() != 0) {
        qca_err("Target operational firmware failed to reach ready state");
        return false;
    }

    /* 7. Start Copy Engine & Connect HTC */
    fCopyEngine->startPipes();
    fHtc->init();
    fHtc->connectService(ATH10K_HTC_EP_1, ATH10K_SERVICE_WMI_CONTROL);
    fHtc->connectService(ATH10K_HTC_EP_2, ATH10K_SERVICE_HTT_DATA);

    /* 8. Initialize WMI Control Plane & HTT Data Plane */
    fWmi->init();
    fWmi->sendInitCmd();
    fHtt->init();

    fFwRunning = true;
    qca_info("QCA9377 firmware boot completed successfully");
    return true;
}

void QCAPCIDevice::start()
{
    if (fFwRunning && fWmi) {
        fWmi->createVdev(0, fMacAddr);
    }
}

void QCAPCIDevice::stop()
{
    if (fFwRunning && fWmi) {
        fWmi->stopVdev(0);
    }
    if (fCopyEngine) {
        fCopyEngine->stopPipes();
    }
}

void QCAPCIDevice::getMacAddress(uint8_t *mac)
{
    if (mac) {
        memcpy(mac, fMacAddr, 6);
    }
}

void QCAPCIDevice::setChannel(uint32_t channel)
{
    if (fWmi) {
        u32 width = (channel > 14) ? 80 : 20; /* 80 MHz for 5GHz, 20 MHz for 2.4GHz */
        fWmi->startVdev(0, channel, width);
    }
}

void QCAPCIDevice::startScan()
{
    if (fWmi) {
        fWmi->startScan(0); /* Active scan */
    }
}

void QCAPCIDevice::associate(apple80211_assoc_data *assoc)
{
    if (fWmi && assoc) {
        fWmi->createPeer(0, assoc->bssid.octet);
        fWmi->associatePeer(0, assoc->bssid.octet, assoc);
    }
}

void QCAPCIDevice::disassociate()
{
    if (fWmi) {
        fWmi->stopVdev(0);
    }
}

void QCAPCIDevice::installKey(apple80211_key *key)
{
    if (fWmi && key) {
        fWmi->installKey(0, key);
    }
}

UInt32 QCAPCIDevice::transmitPacket(mbuf_t m)
{
    if (fHtt) {
        return fHtt->transmitPacket(m);
    }
    return 0;
}

int QCAPCIDevice::transmitRaw80211(mbuf_t m)
{
    if (fHtt) {
        return fHtt->transmitRaw80211(m);
    }
    return 0;
}

void QCAPCIDevice::watchdogCheck()
{
    /* Check firmware alive status */
    u32 val = read32(FW_INDICATOR_ADDRESS);
    if (val & FW_IND_EVENT_PENDING) {
        qca_err("Watchdog detected target crash (0x%08x); initiating recovery", val);
        warmReset();
    }
}
