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

static const u32 readLE32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* Registry breadcrumbs: the driver's IOLog output never reaches the
   unified log, so every bring-up value is mirrored into ioreg. Values
   are stored as hex strings because the numeric setProperty overload
   would silently store them as booleans. */
static void qcaSetHexProp(IOPCIDevice *nub, const char *key, u32 value)
{
    if (!nub) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%08x", value);
    nub->setProperty(key, buf);
}

/* The authoritative firmware indicator is the one ath10k uses for the
   QCA9377 (qca6174_regs.fw_indicator_address = 0x3a028). The second
   candidate is sampled for diagnostics only: it holds the QCA "alive"
   magic 0xdeadc0de on this chip but never changes, so treating it as
   the indicator would mask a target that never came up. */
static const u32 kFwIndicatorCandidates[] = {
    0x0003a028,   /* qca6174 fw_indicator_address (authoritative) */
    0x0000180c,   /* alternate scratch register (diagnostic only) */
};

#define FW_IND_CANDIDATE_COUNT \
    (sizeof(kFwIndicatorCandidates) / sizeof(kFwIndicatorCandidates[0]))

/* Register fingerprint: one read of every interesting block, so a live
   register block can be identified from the values alone. */
static const u32 kRegFingerprint[] = {
    0x00000800,   /* SOC reset control */
    0x00000828,   /* scratch 3 */
    0x000008f0,   /* SoC chip id */
    0x0000180c,   /* alternate indicator */
    0x00034400,   /* CE0 base */
    0x00034410,   /* CE0 control 1 */
    0x00034418,   /* CE0 command */
    0x0003a000,   /* SoC core control */
    0x0003a008,   /* PCIe interrupt enable */
    0x0003a028,   /* firmware indicator */
    0x00080000,   /* RTC state */
    0x00080004,   /* SoC wake */
    0x00080008,   /* global reset */
};

#define REG_FINGERPRINT_COUNT \
    (sizeof(kRegFingerprint) / sizeof(kRegFingerprint[0]))

/* Time-series snapshot: the registers that tell whether the target CPU
   is running and whether the Copy Engine is alive. */
static const u32 kRegSnapshot[] = {
    0x00000800,   /* SOC reset control */
    0x00000828,   /* scratch 3 */
    0x000008f0,   /* SoC chip id */
    0x0000180c,   /* alternate indicator */
    0x00034410,   /* CE0 control 1 */
    0x00034418,   /* CE0 command */
    0x00034430,   /* CE0 watermark interrupt status */
    0x00034444,   /* CE0 source read index */
    0x00034848,   /* CE1 destination read index */
    0x0003a000,   /* SoC core control */
    0x0003a00c,   /* PCIe interrupt cause */
    0x0003a028,   /* firmware indicator */
    0x00080000,   /* RTC state */
    0x00080008,   /* global reset */
};

#define REG_SNAPSHOT_COUNT \
    (sizeof(kRegSnapshot) / sizeof(kRegSnapshot[0]))

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
      fTargetAwake(false),
      fPcieCap(0),
      fL1PMSubstatesCap(0)
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
    qcaSetHexProp(fPciNub, "QCAChipId", chipId);
    u32 fwIndInit = read32(FW_INDICATOR_ADDRESS);
    qcaSetHexProp(fPciNub, "QCAFWIndInit", fwIndInit);
    dumpRegisterFingerprint("Pre");

    /* Disable ASPM before any target reset — ath10k clears
       PCI_EXP_LNKCTL_ASPMC in hif_power_up for the same reason: the
       link must not drop to L1 while the target is being reset and
       polled. */
    UInt8 pcieCap = 0;
    fPciNub->findPCICapability(kIOPCICapabilityIDPCIExpress, &pcieCap);
    fPcieCap = pcieCap;
    if (pcieCap) {
        UInt16 linkCtl = fPciNub->configRead16(pcieCap + 0x10);
        qcaSetHexProp(fPciNub, "QCALinkCtl", linkCtl);
        if (linkCtl & 0x3) {
            fPciNub->configWrite16(pcieCap + 0x10, (UInt16)(linkCtl & ~0x3));
            fPciNub->setProperty("QCAASPM", "disabled");
        } else {
            fPciNub->setProperty("QCAASPM", "already-off");
        }
    } else {
        fPciNub->setProperty("QCAASPM", "no-pcie-cap");
    }

    /* The endpoint advertises L1.1/L1.2 substates. If the link enters
       one of them while the target ROM is booting, the target can wedge
       itself long before the host gets a chance to talk to it. The
       substates are switched off together with ASPM. */
    IOByteCount l1ssOffset = 0;
    if (fPciNub->extendedFindPCICapability(kIOPCIExpressCapabilityIDL1PMSubstates,
                                           &l1ssOffset)) {
        fL1PMSubstatesCap = (UInt32)l1ssOffset;
        qcaSetHexProp(fPciNub, "QCAL1SSCap", fL1PMSubstatesCap);
    }
    applyLinkPowerPolicy();
    dumpPcieState("Pre");
    dumpPcieConfig("Pre");

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
            qcaSetHexProp(fPciNub, "QCAWakeState", state);
            return true;
        }
        qca_mdelay(1);
    }

    qcaSetHexProp(fPciNub, "QCAWakeState", state);
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
    /* Read the register back: if the write does not stick the warm
       reset never happened and the target keeps running the code that
       just crashed. */
    qcaSetHexProp(fPciNub, "QCAWarmRstCtl", read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS));

    /* ath10k_pci_init_pipes: reprogram the CE hardware. */
    fCopyEngine->startPipes();

    /* ath10k ignores the result of this wait: the CE reset and the
       second CPU reset that follow are what actually revive the target,
       so bailing out here would skip half of the sequence. */
    waitForTargetInit(1000);

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
    qcaSetHexProp(fPciNub, "QCAWarmRstCtl2", read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS));

    fCopyEngine->startPipes();

    return waitForTargetInit(1000);
}

int QCAPCIDevice::chipReset()
{
    /* ath10k_pci_cold_reset: put the target including PCIe into reset,
       wait for the PCIe core to become stable, then pull it out again.
       Leaving the reset asserted — as an earlier version did — keeps
       the SoC in reset forever and the ROM bootloader never runs. */
    qca_info("Executing SoC cold reset");
    u32 val = read32(PCIE_LOCAL_BASE_ADDRESS + SOC_GLOBAL_RESET_ADDRESS);
    qcaSetHexProp(fPciNub, "QCADeviceResetReg", val);
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
    qcaSetHexProp(fPciNub, "QCAFWIndPostReset", read32(FW_INDICATOR_ADDRESS));
    dumpRegisterFingerprint("Post");
    snapshotRegisters("Rst");

    /* The cold reset also resets the PCIe core: re-apply the link
       policy and record the resulting link state. */
    applyLinkPowerPolicy();
    dumpPcieState("Rst");
    dumpPcieConfig("Rst");
    return 0;
}

int QCAPCIDevice::waitForTargetInit(int timeoutMs)
{
    qca_info("Waiting for target firmware to initialize");
    u32 last[FW_IND_CANDIDATE_COUNT] = { 0 };
    u32 first[FW_IND_CANDIDATE_COUNT] = { 0 };
    u32 onesCount = 0, totalCount = 0;
    bool haveFirst = false;
    /* Snapshot the interesting registers while waiting so the boot log
       shows exactly when (if ever) the target started. */
    int snapshotAt[] = { 50, 200, 500, 1000, 2000 };
    unsigned snapshotCount = sizeof(snapshotAt) / sizeof(snapshotAt[0]);
    unsigned snapshotIdx = 0;
    int elapsed = 0;
    /* The target reports a crash within a few milliseconds of the reset,
       so the indicator is polled at 1 ms to pin down the exact moment
       the target died. */
    int lastNonZero = -1;

    while (timeoutMs > 0) {
        /* Keep the SoC awake while polling: re-validate the RTC state
           and re-wake if the target dropped back to sleep. */
        u32 rtc = read32Raw(PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS);
        if ((rtc & RTC_STATE_V_MASK) != RTC_STATE_V_ON) {
            markTargetAsleep();
            wakeTarget();
        }

        for (unsigned i = 0; i < FW_IND_CANDIDATE_COUNT; i++) {
            u32 val = read32(kFwIndicatorCandidates[i]);
            if (!haveFirst) first[i] = val;
            last[i] = val;
            if (val == 0xFFFFFFFF) {
                onesCount++;
                continue;
            }
            /* Only the authoritative indicator (index 0) counts as
               "target initialized"; the alternate register is sampled
               for diagnostics only. */
            if (i != 0) continue;
            if (val) lastNonZero = elapsed;
            if (val & FW_IND_EVENT_PENDING) {
                qcaSetHexProp(fPciNub, "QCAFwIndLast", val);
                qcaSetHexProp(fPciNub, "QCAFwIndPolls", totalCount);
                qcaSetHexProp(fPciNub, "QCAFwIndFirst0", first[0]);
                qca_err("Target reported firmware crash during init (0x%08x) after %d ms",
                        val, elapsed);
                captureCrashState(elapsed);
                return -1;
            }
            if (val & FW_IND_INITIALIZED) {
                char key[32];
                snprintf(key, sizeof(key), "QCAFwIndHit%u", i);
                qcaSetHexProp(fPciNub, key, kFwIndicatorCandidates[i]);
                qca_info("Target initialized successfully (indicator %u = 0x%08x)",
                         i, val);
                return 0;
            }
        }

        haveFirst = true;
        totalCount++;
        elapsed += 1;
        if (snapshotIdx < snapshotCount && elapsed >= snapshotAt[snapshotIdx]) {
            char tag[16];
            snprintf(tag, sizeof(tag), "T%d", snapshotAt[snapshotIdx]);
            snapshotRegisters(tag);
            snapshotIdx++;
        }
        qca_mdelay(1);
        timeoutMs -= 1;
    }

    for (unsigned i = 0; i < FW_IND_CANDIDATE_COUNT; i++) {
        char key[32];
        snprintf(key, sizeof(key), "QCAFwIndFirst%u", i);
        qcaSetHexProp(fPciNub, key, first[i]);
        snprintf(key, sizeof(key), "QCAFwIndLast%u", i);
        qcaSetHexProp(fPciNub, key, last[i]);
    }
    qcaSetHexProp(fPciNub, "QCAFwIndOnes", onesCount);
    qcaSetHexProp(fPciNub, "QCAFwIndPolls", totalCount);
    qcaSetHexProp(fPciNub, "QCAFwIndLastNz", (u32)lastNonZero);
    qca_err("Timed out waiting for target initialization");
    return -1;
}

bool QCAPCIDevice::targetInitialized()
{
    u32 val = read32(FW_INDICATOR_ADDRESS);
    qcaSetHexProp(fPciNub, "QCAFwIndNow", val);
    return (val & FW_IND_INITIALIZED) != 0;
}

void QCAPCIDevice::snapshotRegisters(const char *tag)
{
    char key[40];
    for (unsigned i = 0; i < REG_SNAPSHOT_COUNT; i++) {
        u32 val = read32(kRegSnapshot[i]);
        snprintf(key, sizeof(key), "QCA%s%05x", tag, kRegSnapshot[i]);
        qcaSetHexProp(fPciNub, key, val);
    }

    /* PCIe link and device status: a cold reset also resets the PCIe
       core, so a link that does not come back up explains a target that
       never starts. Link status bit 0 = data-layer link active,
       bit 1 = physical link active. */
    u32 linkStatus = 0, devStatus = 0, cmd = 0, bar0 = 0;
    if (fPcieCap) {
        linkStatus = fPciNub->configRead16(fPcieCap + 0x12);
        devStatus = fPciNub->configRead16(fPcieCap + 0x08);
    }
    cmd = fPciNub->configRead16(kIOPCIConfigCommand);
    bar0 = fPciNub->configRead32(kIOPCIConfigBaseAddress0);
    snprintf(key, sizeof(key), "QCA%sLinkSt", tag);
    qcaSetHexProp(fPciNub, key, linkStatus);
    snprintf(key, sizeof(key), "QCA%sDevSt", tag);
    qcaSetHexProp(fPciNub, key, devStatus);
    snprintf(key, sizeof(key), "QCA%sCmd", tag);
    qcaSetHexProp(fPciNub, key, cmd);
    snprintf(key, sizeof(key), "QCA%sBar0", tag);
    qcaSetHexProp(fPciNub, key, bar0);
}

void QCAPCIDevice::setDiagProp(const char *key, u32 value)
{
    qcaSetHexProp(fPciNub, key, value);
}

void QCAPCIDevice::setDiagStr(const char *key, const char *value)
{
    if (fPciNub && value) fPciNub->setProperty(key, value);
}

void QCAPCIDevice::dumpRegisterFingerprint(const char *tag)
{
    /* Mirror a read of every interesting register into ioreg so the
       live register block can be identified from the values alone. */
    char key[32];
    for (unsigned i = 0; i < REG_FINGERPRINT_COUNT; i++) {
        u32 val = read32(kRegFingerprint[i]);
        snprintf(key, sizeof(key), "QCAReg%s%05x", tag, kRegFingerprint[i]);
        qcaSetHexProp(fPciNub, key, val);
    }
}

/* Keep the PCIe link out of the low-power substates while the target
   ROM boots. ath10k clears ASPM in hif_power_up for the same reason and
   never lets the link enter L1 while the target is being reset and
   polled. macOS' PCI stack enables ASPM L1 on this device by default
   (link control bit 1 was set before the driver touched it) and the
   endpoint additionally advertises L1.1/L1.2 substates. */
void QCAPCIDevice::applyLinkPowerPolicy()
{
    if (!fPcieCap) return;

    UInt16 linkCtl = fPciNub->configRead16(fPcieCap + 0x10);
    if (linkCtl & 0x0003) {
        linkCtl = (UInt16)(linkCtl & ~0x0003);
        fPciNub->configWrite16(fPcieCap + 0x10, linkCtl);
    }

    if (fL1PMSubstatesCap) {
        /* L1 PM Substates Control: bit 0 enables the substates. */
        UInt8 ctlOffset = (UInt8)(fL1PMSubstatesCap + 4);
        UInt32 ctl = fPciNub->configRead32(ctlOffset);
        if (ctl & 0x1) {
            ctl &= ~0x1u;
            fPciNub->configWrite32(ctlOffset, ctl);
        }
        qcaSetHexProp(fPciNub, "QCAL1SSCtl", fPciNub->configRead32(ctlOffset));
    }
}

/* PCIe capability state: link and device control/status registers. A
   link that drops into a low-power substate (or that reports a speed
   or width downgrade) explains a target that starts and then dies. */
void QCAPCIDevice::dumpPcieState(const char *tag)
{
    if (!fPcieCap) return;
    char key[40];

    u16 linkCtl  = fPciNub->configRead16(fPcieCap + 0x10);
    u16 linkSta  = fPciNub->configRead16(fPcieCap + 0x12);
    u16 devCtl   = fPciNub->configRead16(fPcieCap + 0x08);
    u16 devSta   = fPciNub->configRead16(fPcieCap + 0x0a);
    u16 linkCtl2 = fPciNub->configRead16(fPcieCap + 0x30);

    snprintf(key, sizeof(key), "QCA%sLinkCtl", tag);
    qcaSetHexProp(fPciNub, key, linkCtl);
    snprintf(key, sizeof(key), "QCA%sLinkSta", tag);
    qcaSetHexProp(fPciNub, key, linkSta);
    snprintf(key, sizeof(key), "QCA%sDevCtl", tag);
    qcaSetHexProp(fPciNub, key, devCtl);
    snprintf(key, sizeof(key), "QCA%sDevSta", tag);
    qcaSetHexProp(fPciNub, key, devSta);
    snprintf(key, sizeof(key), "QCA%sLinkCtl2", tag);
    qcaSetHexProp(fPciNub, key, linkCtl2);
}

/* Full 256 byte dump of the standard configuration space. The values
   can be compared against a Linux boot of the same machine, which is
   the quickest way to spot a power-management or MSI difference
   between the two stacks. */
void QCAPCIDevice::dumpPcieConfig(const char *tag)
{
    char key[40];
    char buf[48];

    for (unsigned row = 0; row < 16; row++) {
        UInt32 d0 = fPciNub->configRead32((UInt8)(row * 16));
        UInt32 d1 = fPciNub->configRead32((UInt8)(row * 16 + 4));
        UInt32 d2 = fPciNub->configRead32((UInt8)(row * 16 + 8));
        UInt32 d3 = fPciNub->configRead32((UInt8)(row * 16 + 12));
        snprintf(buf, sizeof(buf), "%08x %08x %08x %08x", d0, d1, d2, d3);
        snprintf(key, sizeof(key), "QCAcfg%s%02x", tag, row * 16);
        fPciNub->setProperty(key, buf);
    }
}

/* Everything that is known about the target at the moment it reports a
   crash: the firmware indicator, the SoC registers, the PCIe link and
   the configuration space. */
void QCAPCIDevice::captureCrashState(int elapsedMs)
{
    qcaSetHexProp(fPciNub, "QCACrashMs", (u32)elapsedMs);
    qcaSetHexProp(fPciNub, "QCACrashInd", read32(FW_INDICATOR_ADDRESS));
    qcaSetHexProp(fPciNub, "QCACrashCtrl", read32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS));
    qcaSetHexProp(fPciNub, "QCACrashIntrEn", read32(SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS));
    qcaSetHexProp(fPciNub, "QCACrashIntrCause", read32(SOC_CORE_BASE_ADDRESS + PCIE_INTR_CAUSE_ADDRESS));
    qcaSetHexProp(fPciNub, "QCACrashScratch", read32(RTC_SOC_BASE_ADDRESS + SCRATCH_3_ADDRESS));
    qcaSetHexProp(fPciNub, "QCACrashResetCtl", read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS));
    snapshotRegisters("Crash");
    dumpPcieState("Crash");
    dumpPcieConfig("Crash");
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
    setDiagStr("QCAResetSeq", "cold1");
    chipReset();
    fPciNub->setProperty("QCAStartStage", "fw-wait");

    /* 2. Wait for the ROM bootloader after the cold reset. ath10k's
       qca6174 reset needs cold *and* warm reset (its FIXME says "QCA6174
       requires cold + warm reset to work"), and the warm reset is what
       pulses the CPU release. A timeout here is therefore not fatal:
       the warm reset below waits again. */
    if (waitForTargetInit() != 0) {
        qca_info("ROM indicator not seen after cold reset");
        setDiagStr("QCAIndAfterCold", "no");
    } else {
        setDiagStr("QCAIndAfterCold", "yes");
    }

    /* 3. Bring the target up. Several reset shapes are tried because
       ath10k resets the chip twice during probe (once in the PCI probe
       and once in ath10k_pci_hif_power_up); every variant is recorded
       so the next boot shows which one worked. */
    if (!targetInitialized()) {
        setDiagStr("QCAResetSeq", "warm1");
        if (warmReset() != 0) {
            qca_err("Target did not respond after the first warm reset");
            setDiagStr("QCAResetSeq", "cold2");
            chipReset();
            if (waitForTargetInit(1500) != 0) {
                setDiagStr("QCAResetSeq", "warm2");
                warmReset();
            }
        }
    }
    setDiagStr("QCAIndAfterWarm", targetInitialized() ? "yes" : "no");
    snapshotRegisters("Warm");

    fPciNub->setProperty("QCAStartStage", "fw-target-info");

    /* 4. Enable the target CPU's PCIe interrupt line before BMI
       traffic (ath10k_pci_wake_target_cpu). */
    u32 ctrl = read32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS);
    write32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS,
            ctrl | CORE_CTRL_CPU_INTR_MASK);

    /* 5. Query ROM target info. This doubles as the authoritative
       liveness probe: a live ROM answers even if the firmware
       indicator was never written. */
    setDiagStr("QCAStartStage", "fw-bmi-send");
    snapshotRegisters("Bmi");
    struct bmi_target_info info;
    if (fBmi->getTargetInfo(&info) != 0) {
        qca_err("BMI target info exchange failed");
        setDiagStr("QCAStartStage", "fw-bmi-failed");
        return false;
    }
    qca_info("Target Xtensa CPU active: version 0x%08x type 0x%08x", info.version, info.type);
    setDiagStr("QCAStartStage", "fw-bmi-ok");

    /* 6. Enable the PLL clock before bulk transfers
       (ath10k_hw_qca6174_enable_pll_clock; failure is non-fatal). */
    setDiagStr("QCAStartStage", "fw-pll");
    if (enablePllClock() != 0) {
        qca_info("PLL clock config failed; continuing at ROM clock");
    }

    /* 7. Configure the target's shared host-interest settings before
       board and firmware download (ath10k_init_configure_target). */
    setDiagStr("QCAStartStage", "fw-config");
    if (configureTarget() != 0) {
        qca_err("Target configuration failed");
        return false;
    }

    setDiagStr("QCAStartStage", "fw-board");
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

    setDiagStr("QCAStartStage", "fw-otp");
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
    setDiagStr("QCAStartStage", "fw-bmi-done");
    if (fBmi->done() != 0) return false;

    /* Wait for firmware RTOS ready */
    setDiagStr("QCAStartStage", "fw-ready");
    if (waitForTargetInit(3000) != 0) {
        qca_err("Target operational firmware failed to reach ready state");
        return false;
    }

    /* Connect HTC after BMI has handed control to the firmware. */
    setDiagStr("QCAStartStage", "fw-htc");
    if (!fHtc->init() ||
        !fHtc->connectService(ATH10K_HTC_EP_1, ATH10K_SERVICE_WMI_CONTROL) ||
        !fHtc->connectService(ATH10K_HTC_EP_2, ATH10K_SERVICE_HTT_DATA) ||
        !fHtc->start()) {
        qca_err("HTC ready/service setup failed");
        return false;
    }

    /* 8. WMI-TLV firmware announces its memory requirements before INIT. */
    setDiagStr("QCAStartStage", "fw-wmi");
    if (!fWmi->init() || fWmi->waitServiceReady() != 0 || fWmi->sendInitCmd() != 0 ||
        fWmi->waitReady(fMacAddr) != 0) {
        qca_err("WMI-TLV service-ready/init/ready handshake failed");
        return false;
    }
    setDiagStr("QCAStartStage", "fw-htt");
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
