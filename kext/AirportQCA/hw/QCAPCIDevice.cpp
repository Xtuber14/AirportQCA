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

extern "C" unsigned char qca9377_firmware_6_bin[];
extern "C" unsigned int qca9377_firmware_6_bin_len;
extern "C" unsigned char qca9377_board_bin[];
extern "C" unsigned int qca9377_board_bin_len;

static u32 readLE32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static bool findFirmwareImages(const u8 *blob, u32 size, const u8 **otp, u32 *otpLen,
                               const u8 **image, u32 *imageLen)
{
    static const u8 magic[] = "QCA-ATH10K";
    if (!blob || size < sizeof(magic) || memcmp(blob, magic, sizeof(magic)) != 0) return false;
    u32 offset = (sizeof(magic) + 3U) & ~3U;
    while (offset <= size && size - offset >= 8) {
        u32 id = readLE32(blob + offset);
        u32 len = readLE32(blob + offset + 4);
        offset += 8;
        if (len > size - offset) return false;
        if (id == 3) { *image = blob + offset; *imageLen = len; }
        else if (id == 4) { *otp = blob + offset; *otpLen = len; }
        u32 padded = (len + 3U) & ~3U;
        if (padded > size - offset) return false;
        offset += padded;
    }
    return *otp && *otpLen && *image && *imageLen;
}

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
      fFwRunning(false),
      fTargetAwake(false)
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
    fPciNub->setProperty("QCAStartStage", "hal-map");
    fMmioMap = fPciNub->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fMmioMap) {
        qca_err("Failed to map BAR0 MMIO space");
        return false;
    }
    fMmioBase = (volatile void*)fMmioMap->getVirtualAddress();
    qca_info("BAR0 MMIO mapped at virtual address %p (size %llu)",
             fMmioBase, fMmioMap->getLength());

    /* MMIO liveness probes (values become ioreg properties): the SoC
       chip id and the firmware indicator before any reset. */
    u32 chipId = read32(RTC_SOC_BASE_ADDRESS + SOC_CHIP_ID_ADDRESS);
    fPciNub->setProperty("QCAChipId", chipId);
    u32 fwIndInit = read32(FW_INDICATOR_ADDRESS);
    fPciNub->setProperty("QCAFWIndInit", fwIndInit);

    /* Disable ASPM before any target reset — ath10k clears
       PCI_EXP_LNKCTL_ASPMC in hif_power_up for the same reason: the
       link must not drop to L1 while the target is being reset and
       polled. */
    UInt8 pcieCap = 0;
    fPciNub->findPCICapability(kIOPCICapabilityIDPCIExpress, &pcieCap);
    if (pcieCap) {
        UInt16 linkCtl = fPciNub->configRead16(pcieCap + 0x10);
        fPciNub->setProperty("QCALinkCtl", linkCtl);
        if (linkCtl & 0x3) {
            fPciNub->configWrite16(pcieCap + 0x10, (UInt16)(linkCtl & ~0x3));
            fPciNub->setProperty("QCAASPM", "disabled");
        } else {
            fPciNub->setProperty("QCAASPM", "already-off");
        }
    } else {
        fPciNub->setProperty("QCAASPM", "no-pcie-cap");
    }

    /* Initialize Copy Engine */
    fPciNub->setProperty("QCAStartStage", "hal-ce");
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
    fPciNub->setProperty("QCAStartStage", "hal-fw");
    if (!bootFirmware()) {
        qca_err("Firmware boot sequence failed");
        return false;
    }

    return true;
}

bool QCAPCIDevice::wakeTarget()
{
    /* ath10k __ath10k_pci_wake + ath10k_pci_wake_wait: assert the SoC
       wake bit in the PCIe local register block and wait until the RTC
       state register reports ON. While the SoC sleeps its register file
       is unpowered, so reads return zero and the ROM never runs. */
    if (fTargetAwake) return true;
    if (!fMmioBase) return false;

    write32Raw(PCIE_LOCAL_BASE_ADDRESS + PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);

    u32 state = 0;
    for (int i = 0; i < 100; i++) {
        state = read32Raw(PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS);
        if ((state & RTC_STATE_V_MASK) == RTC_STATE_V_ON) {
            fTargetAwake = true;
            fPciNub->setProperty("QCAWakeState", state);
            return true;
        }
        qca_mdelay(1);
    }

    fPciNub->setProperty("QCAWakeState", state);
    fPciNub->setProperty("QCAWakeFail", true);
    qca_err("Target SoC did not wake (RTC state 0x%08x)", state);
    return false;
}

void QCAPCIDevice::markTargetAsleep()
{
    /* The SoC wake state is cleared by a target reset; the next MMIO
       access re-wakes it. */
    fTargetAwake = false;
}

int QCAPCIDevice::warmReset()
{
    /* ath10k_pci_warm_reset for the QCA6174/QCA9377 family: quiesce the
       target CPU, warm-reset it, reprogram the Copy Engine, clear the
       LF timer, reset the CE, and wait for the ROM to signal ready. */
    u32 val;

    /* ath10k_pci_warm_reset_si0 (SI0_RST mask is 0 for qca6174, so the
       writes are value-preserving no-ops; mirrored for fidelity). */
    val = read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS);
    write32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS, val);
    read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS);
    qca_mdelay(10);
    val = read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS);
    write32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS, val);
    read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS);
    qca_mdelay(10);

    /* ath10k_pci_warm_reset_cpu: clear FW indicator and warm-reset CPU. */
    write32(FW_INDICATOR_ADDRESS, 0);
    val = read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS);
    write32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS,
            val | SOC_RESET_CONTROL_CPU_WARM_RST_MASK);

    /* ath10k_pci_init_pipes: reprogram the CE hardware. */
    fCopyEngine->startPipes();

    if (waitForTargetInit() != 0) return -1;

    /* ath10k_pci_warm_reset_clear_lf: disable the LF timer. */
    val = read32(RTC_SOC_BASE_ADDRESS + SOC_LF_TIMER_CONTROL0_ADDRESS);
    write32(RTC_SOC_BASE_ADDRESS + SOC_LF_TIMER_CONTROL0_ADDRESS,
            val & ~SOC_LF_TIMER_CONTROL0_ENABLE_MASK);

    /* ath10k_pci_warm_reset_ce: pulse the CE reset bit. */
    val = read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS);
    write32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS,
            val | SOC_RESET_CONTROL_CE_RST_MASK);
    qca_mdelay(10);
    write32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS,
            val & ~SOC_RESET_CONTROL_CE_RST_MASK);

    /* Second warm_reset_cpu. */
    write32(FW_INDICATOR_ADDRESS, 0);
    val = read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS);
    write32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS,
            val | SOC_RESET_CONTROL_CPU_WARM_RST_MASK);

    fCopyEngine->startPipes();

    return waitForTargetInit();
}

int QCAPCIDevice::chipReset()
{
    /* ath10k_pci_cold_reset: put the target including PCIe into reset,
       wait for the PCIe core to become stable, then pull it out again.
       Leaving the reset asserted — as an earlier version did — keeps
       the SoC in reset forever and the ROM bootloader never runs. */
    qca_info("Executing SoC cold reset");
    u32 val = read32(PCIE_LOCAL_BASE_ADDRESS + SOC_GLOBAL_RESET_ADDRESS);
    fPciNub->setProperty("QCADeviceResetReg", val);
    val |= 1;
    write32(PCIE_LOCAL_BASE_ADDRESS + SOC_GLOBAL_RESET_ADDRESS, val);
    qca_mdelay(20);
    val &= ~1;
    write32(PCIE_LOCAL_BASE_ADDRESS + SOC_GLOBAL_RESET_ADDRESS, val);
    qca_mdelay(20);

    /* The reset cleared the SoC wake state; the next MMIO access will
       wake it again. */
    markTargetAsleep();

    /* Re-assert bus master and memory decode in case the cold reset
       cleared the device's command register; if it did, every MMIO
       read below would return 0xFFFFFFFF. */
    fPciNub->setBusMasterEnable(true);
    fPciNub->setMemoryEnable(true);
    fPciNub->setProperty("QCADecodeReassert", true);

    /* Post-reset liveness probe. */
    fPciNub->setProperty("QCAFWIndPostReset", read32(FW_INDICATOR_ADDRESS));
    return 0;
}

int QCAPCIDevice::waitForTargetInit()
{
    qca_info("Waiting for target firmware to initialize");
    u32 firstVal = 0xFFFFFFFF, lastVal = 0xFFFFFFFF;
    u32 onesCount = 0, totalCount = 0;
    int timeoutMs = 1500;
    while (timeoutMs > 0) {
        /* Keep the SoC awake while polling: re-validate the RTC state
           and re-wake if the target dropped back to sleep. */
        u32 rtc = read32Raw(PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS);
        if ((rtc & RTC_STATE_V_MASK) != RTC_STATE_V_ON) {
            markTargetAsleep();
            wakeTarget();
        }

        u32 val = read32(FW_INDICATOR_ADDRESS);
        if (totalCount == 0) firstVal = val;
        lastVal = val;
        totalCount++;
        if (val == 0xFFFFFFFF) {
            onesCount++;
            qca_mdelay(10);
            timeoutMs -= 10;
            continue;
        }
        if (val & FW_IND_EVENT_PENDING) {
            fPciNub->setProperty("QCAFWIndLast", lastVal);
            fPciNub->setProperty("QCAFWIndPolls", totalCount);
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
    fPciNub->setProperty("QCAFWIndFirst", firstVal);
    fPciNub->setProperty("QCAFWIndLast", lastVal);
    fPciNub->setProperty("QCAFWIndOnes", onesCount);
    fPciNub->setProperty("QCAFWIndPolls", totalCount);
    qca_err("Timed out waiting for target initialization");
    return -1;
}

int QCAPCIDevice::enablePllClock()
{
    /* ath10k_hw_qca6174_enable_pll_clock: configure the PLL from the
       eFuse reference-clock index through BMI SOC-register accesses,
       then program the target RAM PLL block. All steps are
       read-modify-write on the target's RTC registers. */
    const u32 clkDivAddr = QCA6174_CORE_CLK_DIV_ADDRESS;
    const u32 pllInitAddr = QCA6174_CPU_PLL_INIT_ADDRESS;
    const u32 speedAddr = QCA6174_CPU_SPEED_ADDRESS;
    u32 regVal = 0, memVal = 0;
    int ret;

    ret = fBmi->readSocRegister(RTC_SOC_BASE_ADDRESS | EFUSE_OFFSET, &regVal);
    if (ret != 0) return ret;

    u32 idx = QCA_MS(regVal, EFUSE_XTAL_SEL_MASK);
    if (idx >= QCA_REFCLK_COUNT) {
        qca_err("eFuse refclk index %u out of range", idx);
        return -1;
    }
    const struct qca_hw_clk_params *hwClk = &qca_refclk_params[idx];
    qca_info("Configuring PLL for %u Hz reference clock", hwClk->refclk);

    /* BB PLL config: rnfrac and outdiv. */
    ret = fBmi->readSocRegister(RTC_SOC_BASE_ADDRESS | BB_PLL_CONFIG_OFFSET, &regVal);
    if (ret != 0) return ret;
    regVal &= ~(BB_PLL_CONFIG_FRAC_MASK | BB_PLL_CONFIG_OUTDIV_MASK);
    regVal |= hwClk->rnfrac | (hwClk->outdiv << _qcaLsb(BB_PLL_CONFIG_OUTDIV_MASK));
    ret = fBmi->writeSocRegister(RTC_SOC_BASE_ADDRESS | BB_PLL_CONFIG_OFFSET, regVal);
    if (ret != 0) return ret;

    /* WLAN PLL settle time. */
    ret = fBmi->readSocRegister(RTC_WMAC_BASE_ADDRESS | WLAN_PLL_SETTLE_OFFSET, &regVal);
    if (ret != 0) return ret;
    regVal &= ~WLAN_PLL_SETTLE_TIME_MASK;
    regVal |= hwClk->settleTime;
    ret = fBmi->writeSocRegister(RTC_WMAC_BASE_ADDRESS | WLAN_PLL_SETTLE_OFFSET, regVal);
    if (ret != 0) return ret;

    /* Core clock control divider. */
    ret = fBmi->readSocRegister(RTC_SOC_BASE_ADDRESS | SOC_CORE_CLK_CTRL_OFFSET, &regVal);
    if (ret != 0) return ret;
    regVal &= ~SOC_CORE_CLK_CTRL_DIV_MASK;
    regVal |= 1;
    ret = fBmi->writeSocRegister(RTC_SOC_BASE_ADDRESS | SOC_CORE_CLK_CTRL_OFFSET, regVal);
    if (ret != 0) return ret;

    /* Clock divider register in target RAM. */
    memVal = 1;
    ret = fBmi->writeMem(clkDivAddr, &memVal, sizeof(memVal));
    if (ret != 0) return ret;

    /* PLL control: refdiv, div, NOPWD. */
    ret = fBmi->readSocRegister(RTC_WMAC_BASE_ADDRESS | WLAN_PLL_CONTROL_OFFSET, &regVal);
    if (ret != 0) return ret;
    regVal |= (hwClk->refdiv << _qcaLsb(WLAN_PLL_CONTROL_REFDIV_MASK)) |
              hwClk->div | WLAN_PLL_CONTROL_NOPWD_MASK;
    ret = fBmi->writeSocRegister(RTC_WMAC_BASE_ADDRESS | WLAN_PLL_CONTROL_OFFSET, regVal);
    if (ret != 0) return ret;

    /* Wait for the RTC PLL to stop changing (max ~1 s each pass). */
    const u32 pllControlAddr = RTC_WMAC_BASE_ADDRESS | WLAN_PLL_CONTROL_OFFSET;
    const u32 syncAddr = RTC_WMAC_BASE_ADDRESS | RTC_SYNC_STATUS_OFFSET;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) {
            /* Unset the PLL bypass between the two waits. */
            ret = fBmi->readSocRegister(pllControlAddr, &regVal);
            if (ret != 0) return ret;
            regVal &= ~WLAN_PLL_CONTROL_BYPASS_MASK;
            ret = fBmi->writeSocRegister(pllControlAddr, regVal);
            if (ret != 0) return ret;
        }
        int waitLimit = 1000;
        do {
            ret = fBmi->readSocRegister(syncAddr, &regVal);
            if (ret != 0) return ret;
            if (!(regVal & RTC_SYNC_STATUS_PLL_CHANGING_MASK)) break;
            IOSleep(1);
        } while (--waitLimit > 0);
        if (regVal & RTC_SYNC_STATUS_PLL_CHANGING_MASK) {
            qca_err("RTC PLL never settled (sync status 0x%08x)", regVal);
            return -1;
        }
    }

    /* Hardware CPU clock: standard clock source. */
    ret = fBmi->readSocRegister(RTC_SOC_BASE_ADDRESS | SOC_CPU_CLOCK_OFFSET, &regVal);
    if (ret != 0) return ret;
    regVal &= ~SOC_CPU_CLOCK_STANDARD_MASK;
    regVal |= 1;
    ret = fBmi->writeSocRegister(RTC_SOC_BASE_ADDRESS | SOC_CPU_CLOCK_OFFSET, regVal);
    if (ret != 0) return ret;

    /* Unset NOPWD. */
    ret = fBmi->readSocRegister(pllControlAddr, &regVal);
    if (ret != 0) return ret;
    regVal &= ~WLAN_PLL_CONTROL_NOPWD_MASK;
    ret = fBmi->writeSocRegister(pllControlAddr, regVal);
    if (ret != 0) return ret;

    /* Enable the PLL init and set the target CPU frequency. */
    memVal = 1;
    ret = fBmi->writeMem(pllInitAddr, &memVal, sizeof(memVal));
    if (ret != 0) return ret;
    memVal = QCA6174_TARGET_CPU_FREQ;
    ret = fBmi->writeMem(speedAddr, &memVal, sizeof(memVal));
    if (ret != 0) return ret;

    qca_info("PLL enabled: target CPU clock set to %u Hz", QCA6174_TARGET_CPU_FREQ);
    return 0;
}

int QCAPCIDevice::configureTarget()
{
    /* ath10k_init_configure_target: HTC protocol version, firmware
       mode flags, and host byte-order announcements in host interest. */
    u32 memVal = 0;

    memVal = HTC_PROTOCOL_VERSION;
    if (fBmi->writeMem(QCA_HOST_INTEREST + HI_APP_HOST_INTEREST_OFFSET,
                       &memVal, sizeof(memVal)) != 0) return -1;

    if (fBmi->readMem(QCA_HOST_INTEREST + HI_OPTION_FLAG_OFFSET, &memVal, sizeof(memVal)) != 0)
        return -1;
    memVal |= (1U << HI_OPTION_NUM_DEV_SHIFT) |
              (HI_OPTION_FW_MODE_AP << HI_OPTION_FW_MODE_SHIFT) |
              (1U << HI_OPTION_MAC_ADDR_METHOD_SHIFT);
    if (fBmi->writeMem(QCA_HOST_INTEREST + HI_OPTION_FLAG_OFFSET, &memVal, sizeof(memVal)) != 0)
        return -1;

    memVal = 0;
    if (fBmi->writeMem(QCA_HOST_INTEREST + HI_BE_OFFSET, &memVal, sizeof(memVal)) != 0) return -1;
    if (fBmi->writeMem(QCA_HOST_INTEREST + HI_FW_SWAP_OFFSET, &memVal, sizeof(memVal)) != 0) return -1;

    return 0;
}

bool QCAPCIDevice::bootFirmware()
{
    /* 1. Cold reset (ath10k_pci_cold_reset). */
    chipReset();
    fPciNub->setProperty("QCAStartStage", "fw-wait");

    /* 2. Wait for ROM bootloader after the cold reset. */
    if (waitForTargetInit() != 0) {
        qca_err("Target ROM bootloader failed to respond");
        return false;
    }

    /* 3. Warm reset — ath10k's FIXME states QCA6174 requires cold +
       warm reset to work; the warm reset also reprograms the CE and
       leaves the target ROM ready. */
    if (warmReset() != 0) {
        qca_err("Target failed to respond after warm reset");
        return false;
    }

    fPciNub->setProperty("QCAStartStage", "fw-target-info");

    /* 4. Enable the target CPU's PCIe interrupt line before BMI
       traffic (ath10k_pci_wake_target_cpu). */
    u32 ctrl = read32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS);
    write32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS,
            ctrl | CORE_CTRL_CPU_INTR_MASK);

    /* 5. Query ROM target info. */
    struct bmi_target_info info;
    if (fBmi->getTargetInfo(&info) != 0) return false;
    qca_info("Target Xtensa CPU active: version 0x%08x type 0x%08x", info.version, info.type);

    /* 6. Enable the PLL clock before bulk transfers
       (ath10k_hw_qca6174_enable_pll_clock; failure is non-fatal). */
    fPciNub->setProperty("QCAStartStage", "fw-clock");
    if (enablePllClock() != 0) {
        qca_info("PLL clock config failed; continuing at ROM clock");
    }

    /* 7. Configure the target's shared host-interest settings before
       board and firmware download (ath10k_init_configure_target). */
    if (configureTarget() != 0) {
        qca_err("Target configuration failed");
        return false;
    }

    fPciNub->setProperty("QCAStartStage", "fw-board");
    /* board.bin is ath10k API-1 calibration data for the QCA9377 hw1.0 target. */
    u32 boardAddress = 0;
    u32 boardInitialized = 1;
    if (qca9377_board_bin_len != QCA9377_BOARD_DATA_SZ ||
        fBmi->readMem(QCA_HOST_INTEREST + HI_BOARD_DATA_OFFSET, &boardAddress, sizeof(boardAddress)) != 0 ||
        !boardAddress || fBmi->writeMem(boardAddress, qca9377_board_bin, qca9377_board_bin_len) != 0 ||
        fBmi->writeMem(QCA_HOST_INTEREST + HI_BOARD_DATA_INITIALIZED_OFFSET, &boardInitialized, sizeof(boardInitialized)) != 0) {
        qca_err("Unable to install QCA9377 board calibration data");
        return false;
    }

    fPciNub->setProperty("QCAStartStage", "fw-otp");
    /* Parse the ath10k firmware container and upload OTP then operational image. */
    const u8 *otp = nullptr, *image = nullptr;
    u32 otpLen = 0, imageLen = 0;
    if (!findFirmwareImages(qca9377_firmware_6_bin, qca9377_firmware_6_bin_len,
                            &otp, &otpLen, &image, &imageLen)) {
        qca_err("Malformed QCA9377 firmware-6 container");
        return false;
    }
    u32 otpResult = 0;
    if (fBmi->fastDownload(QCA9377_HW_1_0_PATCH_LOAD_ADDR, otp, otpLen) != 0 ||
        fBmi->execute(QCA9377_HW_1_0_PATCH_LOAD_ADDR, 0, &otpResult) != 0 || otpResult != 0 ||
        fBmi->fastDownload(QCA9377_HW_1_0_PATCH_LOAD_ADDR, image, imageLen) != 0) {
        qca_err("QCA9377 OTP or firmware download failed");
        return false;
    }

    /* Complete BMI Phase */
    if (fBmi->done() != 0) return false;

    /* Wait for firmware RTOS ready */
    if (waitForTargetInit() != 0) {
        qca_err("Target operational firmware failed to reach ready state");
        return false;
    }

    /* Connect HTC after BMI has handed control to the firmware. */
    if (!fHtc->init() ||
        !fHtc->connectService(ATH10K_HTC_EP_1, ATH10K_SERVICE_WMI_CONTROL) ||
        !fHtc->connectService(ATH10K_HTC_EP_2, ATH10K_SERVICE_HTT_DATA) ||
        !fHtc->start()) {
        qca_err("HTC ready/service setup failed");
        return false;
    }

    /* 8. WMI-TLV firmware announces its memory requirements before INIT. */
    if (!fWmi->init() || fWmi->waitServiceReady() != 0 || fWmi->sendInitCmd() != 0 ||
        fWmi->waitReady(fMacAddr) != 0) {
        qca_err("WMI-TLV service-ready/init/ready handshake failed");
        return false;
    }
    if (!fHtt->init()) {
        qca_err("HTT version negotiation/RX ring setup failed");
        return false;
    }

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
    /* The channel ioctl records the selected channel in AirportQCA. Start the
     * station VDEV only when association supplies its SSID and BSSID; starting
     * it here leaves firmware in a started state before a real join request. */
    qca_info("Selected channel %u; firmware VDEV will start during association", channel);
}

int QCAPCIDevice::startScan()
{
    return fWmi ? fWmi->startScan(0) : kIOReturnNotReady;
}

int QCAPCIDevice::associate(apple80211_assoc_data *assoc, u32 channel)
{
    if (!fWmi || !assoc || !channel) return kIOReturnBadArgument;
    const u32 width = channel > 14 ? 80 : 20;
    int result = fWmi->startVdev(0, channel, width, assoc->ad_ssid, assoc->ad_ssid_len);
    if (result != 0) return result;
    result = fWmi->createPeer(0, assoc->ad_bssid.octet);
    if (result != 0) return result;
    return fWmi->associatePeer(0, assoc->ad_bssid.octet, assoc);
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
    if (fWmi) fWmi->pollEvents();
    if (fHtt) fHtt->receivePackets();
    /* Check firmware alive status */
    u32 val = read32(FW_INDICATOR_ADDRESS);
    if (val & FW_IND_EVENT_PENDING) {
        qca_err("Watchdog detected target crash (0x%08x); initiating recovery", val);
        warmReset();
    }
}

void QCAPCIDevice::notifyScanComplete()
{
    if (fDriver) fDriver->notifyScanComplete();
}

void QCAPCIDevice::reportScanBeacon(const u8 *frame, u32 frameLength, u32 channel, u32 snr)
{
    if (fDriver) fDriver->reportScanBeacon(frame, frameLength, channel, snr);
}

void QCAPCIDevice::freePacket(mbuf_t packet)
{
    if (packet && fDriver) fDriver->freePacket(packet);
}
