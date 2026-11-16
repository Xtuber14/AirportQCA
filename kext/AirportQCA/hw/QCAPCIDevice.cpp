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

/* Insert the experiment tag behind the "QCA" prefix of a diagnostic
   property name, so that a single boot can run several reset
   experiments without one overwriting the results of the other. */
static void qcaTaggedKey(char *out, size_t outLen, const char *key, const char *tag)
{
    if (!out || !outLen) return;
    out[0] = '\0';
    if (!key) return;

    if (tag && tag[0]) {
        const char *rest = (strncmp(key, "QCA", 3) == 0) ? key + 3 : key;
        snprintf(out, outLen, "QCA%s%s", tag, rest);
    } else {
        snprintf(out, outLen, "%s", key);
    }
}

/* The authoritative firmware indicator is the one ath10k uses for the
   QCA9377 (qca6174_regs.fw_indicator_address = 0x3a028). A second
   candidate used to be sampled at 0x180c; that address is not part of
   the QCA9377 register map and answers with the poison value 0xdeadc0de.
   The firmware indicator changed from 0 to "event pending" immediately
   after the driver had touched that address, so the access is dropped:
   ath10k never reads it either and an access to an unimplemented MMIO
   address can upset the target ROM. */
static const u32 kFwIndicatorCandidates[] = {
    0x0003a028,   /* qca6174 fw_indicator_address (authoritative) */
};

#define FW_IND_CANDIDATE_COUNT \
    (sizeof(kFwIndicatorCandidates) / sizeof(kFwIndicatorCandidates[0]))

/* Register fingerprint: one read of every interesting block, so a live
   register block can be identified from the values alone. */
static const u32 kRegFingerprint[] = {
    0x00000800,   /* SOC reset control */
    0x00000828,   /* scratch 3 */
    0x000008f0,   /* SoC chip id */
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
    0x00034410,   /* CE0 control 1 */
    0x00034418,   /* CE0 command */
    0x00034430,   /* CE0 watermark interrupt status */
    0x00034434,   /* CE0 misc interrupt enable (error bits) */
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

/* Copy Engine error and status registers. The "misc interrupt enable"
   register carries the CE error bits (axi error, destination address
   error, source length error, destination length violation and the two
   overflow flags); they tell whether the host and the target disagreed
   about the ring layout. */
static const u32 kCeErrorRegisters[] = {
    0x00034008,   /* CE wrapper interrupt summary */
    0x00034418,   /* CE0 command */
    0x00034434,   /* CE0 misc interrupt enable */
    0x00034810,   /* CE1 control 1 */
    0x00034818,   /* CE1 command */
    0x00034834,   /* CE1 misc interrupt enable */
    0x00035034,   /* CE3 misc interrupt enable */
    0x00035434,   /* CE4 misc interrupt enable */
};

#define CE_ERROR_REGISTER_COUNT \
    (sizeof(kCeErrorRegisters) / sizeof(kCeErrorRegisters[0]))

/* Maximum number of firmware indicator value changes that are recorded
   while the driver waits for the target. */
#define FW_IND_SEQ_MAX                     32

/* Time the driver keeps polling the firmware indicator after the target
   raised the event-pending bit. The event is not fatal on its own: the
   BMI exchange is the authoritative liveness probe. */
#define FW_IND_CRASH_GRACE_MS              200

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
    fDiagTag[0] = '\0';
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
    setDiagProp("QCAChipId", chipId);
    u32 fwIndInit = read32(FW_INDICATOR_ADDRESS);
    setDiagProp("QCAFWIndInit", fwIndInit);
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
        setDiagProp("QCALinkCtl", linkCtl);
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
        setDiagProp("QCAL1SSCap", fL1PMSubstatesCap);
    }
    applyLinkPowerPolicy();
    dumpPcieState("Pre");
    dumpPcieConfig("Pre");
    dumpAerState("Pre");

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

/* Strong force-wake (mirrors ath10k_pci_force_wake): always assert SOC_WAKE
   and wait for RTC ON, regardless of current state. Call this before any
   target reset to ensure the SoC is fully powered. */
bool QCAPCIDevice::forceWake()
{
    qca_info("Force-waking target SoC");
    write32Raw(PCIE_LOCAL_BASE_ADDRESS + PCIE_SOC_WAKE_ADDRESS, PCIE_SOC_WAKE_V_MASK);
    for (int i = 0; i < 100; i++) {
        u32 state = read32Raw(PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS);
        if ((state & RTC_STATE_V_MASK) == RTC_STATE_V_ON) {
            setDiagProp("QCAWakeState", state);
            return true;
        }
        qca_mdelay(1);
    }
    u32 state = read32Raw(PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS);
    setDiagProp("QCAWakeState", state);
    qca_err("Target SoC did not wake (RTC state 0x%08x)", state);
    return false;
}

/* Legacy wakeTarget: now just calls forceWake for ABI compatibility.
   The read32/write32 inline functions still call this. */
bool QCAPCIDevice::wakeTarget()
{
    return forceWake();
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
    setDiagProp("QCAWarmRstCtl", read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS));

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
    setDiagProp("QCAWarmRstCtl2", read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS));

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
    setDiagProp("QCADeviceResetReg", val);
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
    setDiagProp("QCAFWIndPostReset", read32(FW_INDICATOR_ADDRESS));
    dumpRegisterFingerprint("Post");
    snapshotRegisters("Rst");

    /* The cold reset also resets the PCIe core: re-apply the link
       policy and record the resulting link state. */
    applyLinkPowerPolicy();
    dumpPcieState("Rst");
    dumpPcieConfig("Rst");
    dumpAerState("Rst");
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
    /* A crash indication is recorded but not acted upon immediately: the
       indicator may also be raised by a recoverable event, and the BMI
       exchange is the authoritative liveness probe. The wait continues
       for a short grace period so the sequence of indicator values is
       captured. */
    bool crashSeen = false;
    int crashGraceMs = 0;
    /* History of the indicator: every value change with the millisecond
       it was seen in. */
    u32 seqValue[FW_IND_SEQ_MAX];
    u32 seqMs[FW_IND_SEQ_MAX];
    unsigned seqCount = 0;
    u32 prevValue = 0;
    bool havePrev = false;

    while (timeoutMs > 0) {
        /* Keep the SoC awake while polling: re-validate the RTC state
           and re-wake if the target dropped back to sleep. */
        u32 rtc = read32Raw(PCIE_LOCAL_BASE_ADDRESS + RTC_STATE_ADDRESS);
        if ((rtc & RTC_STATE_V_MASK) != RTC_STATE_V_ON) {
            if (!forceWake()) {
                qca_err("Failed to re-wake target during polling");
                return -1;
            }
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

            /* Record every change of the indicator together with the
               time it was seen. */
            if (!havePrev || val != prevValue) {
                if (seqCount < FW_IND_SEQ_MAX) {
                    seqValue[seqCount] = val;
                    seqMs[seqCount] = (u32)elapsed;
                    seqCount++;
                }
                prevValue = val;
                havePrev = true;
            }

            if (val & FW_IND_EVENT_PENDING) {
                if (!crashSeen) {
                    crashSeen = true;
                    crashGraceMs = FW_IND_CRASH_GRACE_MS;
                    setDiagProp("QCAFwIndLast", val);
                    setDiagProp("QCAFwIndPolls", totalCount);
                    setDiagProp("QCAFwIndFirst0", first[0]);
                    qca_err("Target reported a firmware event (0x%08x) after %d ms",
                            val, elapsed);
                    captureCrashState(elapsed);
                }
                /* Keep polling: the target may still reach the
                   initialized state, and the BMI exchange decides
                   whether the target is really dead. */
                continue;
            }
            if (val & FW_IND_INITIALIZED) {
                char key[32];
                snprintf(key, sizeof(key), "QCAFwIndHit%u", i);
                setDiagProp(key, kFwIndicatorCandidates[i]);
                qca_info("Target initialized successfully (indicator %u = 0x%08x)",
                         i, val);
                recordIndicatorSequence(seqValue, seqMs, seqCount);
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
        if (crashSeen) {
            crashGraceMs -= 1;
            if (crashGraceMs <= 0) break;
        }
    }

    for (unsigned i = 0; i < FW_IND_CANDIDATE_COUNT; i++) {
        char key[32];
        snprintf(key, sizeof(key), "QCAFwIndFirst%u", i);
        setDiagProp(key, first[i]);
        snprintf(key, sizeof(key), "QCAFwIndLast%u", i);
        setDiagProp(key, last[i]);
    }
    setDiagProp("QCAFwIndOnes", onesCount);
    setDiagProp("QCAFwIndPolls", totalCount);
    setDiagProp("QCAFwIndLastNz", (u32)lastNonZero);
    setDiagProp("QCAFwIndCrash", (u32)(crashSeen ? 1 : 0));
    recordIndicatorSequence(seqValue, seqMs, seqCount);
    if (crashSeen) {
        qca_err("Target did not initialize after the firmware event");
        return -1;
    }
    qca_err("Timed out waiting for target initialization");
    return -1;
}

/* Store the recorded indicator sequence in the registry so the next boot
   shows exactly how the firmware indication evolved after the reset. */
void QCAPCIDevice::recordIndicatorSequence(const u32 *values, const u32 *times,
                                            unsigned count)
{
    char key[40];
    for (unsigned i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "QCAFwIndSeq%u", i);
        setDiagProp(key, values[i]);
        snprintf(key, sizeof(key), "QCAFwIndSeqMs%u", i);
        setDiagProp(key, times[i]);
    }
    setDiagProp("QCAFwIndSeqLen", (u32)count);
}

bool QCAPCIDevice::targetInitialized()
{
    u32 val = read32(FW_INDICATOR_ADDRESS);
    setDiagProp("QCAFwIndNow", val);
    return (val & FW_IND_INITIALIZED) != 0;
}

void QCAPCIDevice::snapshotRegisters(const char *tag)
{
    char key[40];
    for (unsigned i = 0; i < REG_SNAPSHOT_COUNT; i++) {
        u32 val = read32(kRegSnapshot[i]);
        snprintf(key, sizeof(key), "QCA%s%05x", tag, kRegSnapshot[i]);
        setDiagProp(key, val);
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
    setDiagProp(key, linkStatus);
    snprintf(key, sizeof(key), "QCA%sDevSt", tag);
    setDiagProp(key, devStatus);
    snprintf(key, sizeof(key), "QCA%sCmd", tag);
    setDiagProp(key, cmd);
    snprintf(key, sizeof(key), "QCA%sBar0", tag);
    setDiagProp(key, bar0);
}

void QCAPCIDevice::setDiagProp(const char *key, u32 value)
{
    char tagged[64];
    qcaTaggedKey(tagged, sizeof(tagged), key, fDiagTag);
    qcaSetHexProp(fPciNub, tagged, value);
}

void QCAPCIDevice::setDiagStr(const char *key, const char *value)
{
    if (!fPciNub || !value) return;
    char tagged[64];
    qcaTaggedKey(tagged, sizeof(tagged), key, fDiagTag);
    fPciNub->setProperty(tagged, value);
}

void QCAPCIDevice::setDiagTag(const char *tag)
{
    if (tag) {
        strncpy(fDiagTag, tag, sizeof(fDiagTag) - 1);
        fDiagTag[sizeof(fDiagTag) - 1] = '\0';
    } else {
        fDiagTag[0] = '\0';
    }
}

void QCAPCIDevice::dumpRegisterFingerprint(const char *tag)
{
    /* Mirror a read of every interesting register into ioreg so the
       live register block can be identified from the values alone. */
    char key[32];
    for (unsigned i = 0; i < REG_FINGERPRINT_COUNT; i++) {
        u32 val = read32(kRegFingerprint[i]);
        snprintf(key, sizeof(key), "QCAReg%s%05x", tag, kRegFingerprint[i]);
        setDiagProp(key, val);
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
        setDiagProp("QCAL1SSCtl", fPciNub->configRead32(ctlOffset));
    }
}

/* Locate the PCIe Advanced Error Reporting capability. It lives in the
   extended capability space and reports every PCIe error the endpoint
   saw; its header log holds the address of the offending transaction,
   which is the quickest way to find out what killed the target ROM. */
UInt32 QCAPCIDevice::findAerCapability()
{
    IOByteCount offset = 0;
    UInt32 guard = 0;

    while (guard++ < 32) {
        UInt32 dw = fPciNub->extendedConfigRead32(offset);
        UInt16 id = (UInt16)(dw & 0xffff);
        UInt16 next = (UInt16)((dw >> PCI_EXT_CAP_NEXT_SHIFT) & PCI_EXT_CAP_NEXT_MASK);
        if (id == 0x0001) return offset;  // kIOPCIExpressCapabilityIDErrorReporting
        if (!next) break;
        offset = (offset + next + 3) & ~3u;
    }
    return 0;
}

/* Dump the AER capability: error status, mask and severity registers
   plus the header log that identifies the failing transaction. */
void QCAPCIDevice::dumpAerState(const char *tag)
{
    char key[40];
    IOByteCount aer = (IOByteCount)findAerCapability();

    setDiagProp("QCAAerCap", (u32)aer);
    if (!aer) return;

    setDiagProp("QCAAerUncorStatus",
                   fPciNub->extendedConfigRead32(aer + PCI_EXT_AER_UNCOR_STATUS));
    setDiagProp("QCAAerUncorMask",
                   fPciNub->extendedConfigRead32(aer + PCI_EXT_AER_UNCOR_MASK));
    setDiagProp("QCAAerUncorSeverity",
                   fPciNub->extendedConfigRead32(aer + PCI_EXT_AER_UNCOR_SEVERITY));
    setDiagProp("QCAAerCorStatus",
                   fPciNub->extendedConfigRead32(aer + PCI_EXT_AER_COR_STATUS));
    setDiagProp("QCAAerCorMask",
                   fPciNub->extendedConfigRead32(aer + PCI_EXT_AER_COR_MASK));
    setDiagProp("QCAAerCorSeverity",
                   fPciNub->extendedConfigRead32(aer + PCI_EXT_AER_COR_SEVERITY));

    /* Header log: the first dword holds the faulting address, the
       second the requester id and the last two the requester data. */
    for (unsigned i = 0; i < 4; i++) {
        snprintf(key, sizeof(key), "QCAAerLog%u", i);
        setDiagProp(key,
                       fPciNub->extendedConfigRead32(aer + PCI_EXT_AER_HEADER_LOG + i * 4));
    }

    /* The extended capability list itself, so a capability that macOS
       added (or that the device exposes) is visible as well. */
    for (unsigned row = 0; row < 12; row++) {
        IOByteCount addr = PCI_EXT_CAP_START + row * 16;
        if (addr >= PCI_EXT_CAP_END) break;
        UInt32 d0 = fPciNub->extendedConfigRead32(addr);
        UInt32 d1 = fPciNub->extendedConfigRead32(addr + 4);
        UInt32 d2 = fPciNub->extendedConfigRead32(addr + 8);
        UInt32 d3 = fPciNub->extendedConfigRead32(addr + 12);
        char buf[48];
        snprintf(buf, sizeof(buf), "%08x %08x %08x %08x", d0, d1, d2, d3);
        snprintf(key, sizeof(key), "QCAext%s%02x", tag, row * 16);
        fPciNub->setProperty(key, buf);
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
    setDiagProp(key, linkCtl);
    snprintf(key, sizeof(key), "QCA%sLinkSta", tag);
    setDiagProp(key, linkSta);
    snprintf(key, sizeof(key), "QCA%sDevCtl", tag);
    setDiagProp(key, devCtl);
    snprintf(key, sizeof(key), "QCA%sDevSta", tag);
    setDiagProp(key, devSta);
    snprintf(key, sizeof(key), "QCA%sLinkCtl2", tag);
    setDiagProp(key, linkCtl2);
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
    char key[40];
    setDiagProp("QCACrashMs", (u32)elapsedMs);
    setDiagProp("QCACrashInd", read32(FW_INDICATOR_ADDRESS));
    setDiagProp("QCACrashCtrl", read32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS));
    setDiagProp("QCACrashIntrEn", read32(SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS));
    setDiagProp("QCACrashIntrCause", read32(SOC_CORE_BASE_ADDRESS + PCIE_INTR_CAUSE_ADDRESS));
    setDiagProp("QCACrashScratch", read32(RTC_SOC_BASE_ADDRESS + SCRATCH_3_ADDRESS));
    setDiagProp("QCACrashResetCtl", read32(RTC_SOC_BASE_ADDRESS + SOC_RESET_CONTROL_ADDRESS));

    /* Copy Engine error and status registers: they show whether the
       host and the target disagreed about the ring layout. */
    for (unsigned i = 0; i < CE_ERROR_REGISTER_COUNT; i++) {
        snprintf(key, sizeof(key), "QCACrashCE%05x", kCeErrorRegisters[i]);
        setDiagProp(key, read32(kCeErrorRegisters[i]));
    }

    snapshotRegisters("Crash");
    dumpPcieState("Crash");
    dumpPcieConfig("Crash");
    dumpAerState("Crash");
}

/* Dump the target's view of the PCIe setup (struct pcie_state). This
   reveals what the target thinks about the MSI grant, the message
   address/data and the power management method. A target that never
   sees a non-zero msi_granted will never leave the bootloader. */
void QCAPCIDevice::dumpTargetPcieState(const char *tag)
{
    if (!fCopyEngine) return;
    u32 interconnectAddr = QCA_HOST_INTEREST + HI_INTERCONNECT_STATE_OFFSET;
    u32 pcieStateAddr = 0;
    if (fCopyEngine->diagReadMem(interconnectAddr, &pcieStateAddr, sizeof(pcieStateAddr)) != 0) return;
    if (!pcieStateAddr) return;

    char key[40];
    for (unsigned i = 0; i < PCIE_STATE_WORD_COUNT; i++) {
        u32 val = 0;
        if (fCopyEngine->diagReadMem(pcieStateAddr + i * 4, &val, sizeof(val)) != 0) continue;
        snprintf(key, sizeof(key), "QCA%sTgtPcie%u", tag, i);
        setDiagProp(key, val);
    }
}

/* Enable the SoC core PCIe interrupt enable register (ath10k_pci_init_irq_intx).
   The endpoint uses this register to raise the firmware and Copy Engine
   interrupts to the host. */
void QCAPCIDevice::enablePcieInterrupts()
{
    write32(SOC_CORE_BASE_ADDRESS + PCIE_INTR_ENABLE_ADDRESS,
            PCIE_INTR_FIRMWARE_MASK | PCIE_INTR_CE_MASK_ALL);
}

/* Assert or de-assert the CPU interrupt mask in CORE_CTRL (ath10k_pci_wake_target_cpu).
   The target ROM may wait for this interrupt before advancing to INITIALIZED. */
void QCAPCIDevice::setCoreCtrlIntr(bool assert)
{
    u32 ctrl = read32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS);
    if (assert) ctrl |= CORE_CTRL_CPU_INTR_MASK;
    else ctrl &= ~CORE_CTRL_CPU_INTR_MASK;
    write32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS, ctrl);
}

/* Clear the PCI Device Control bit 13 (Disable INTx). macOS never programs
   the MSI capability on this endpoint, so the device is left without any
   interrupt path. */
void QCAPCIDevice::clearDisableIntx()
{
    if (!fPcieCap) return;
    u16 devCtl = fPciNub->configRead16(fPcieCap + 0x08);
    if (devCtl & PCI_EXP_DEVCTL_DISABLE_INTX) {
        devCtl &= ~PCI_EXP_DEVCTL_DISABLE_INTX;
        fPciNub->configWrite16(fPcieCap + 0x08, devCtl);
    }
}

/* Walk the standard PCI capability list for a capability id. */
UInt8 QCAPCIDevice::findStandardCapability(UInt8 capabilityId)
{
    UInt8 capPtr = 0;
    fPciNub->findPCICapability(capabilityId, &capPtr);
    return capPtr;
}

/* Convert a target CPU address into the address space the Copy Engine
   uses (ath10k_pci_qca6174_targ_cpu_to_ce_addr). The base of the
   interconnect window comes from the SoC core control register; bit 20
   of the result selects the target space above 1 MB. */
u32 QCAPCIDevice::targCpuToCeAddr(u32 addr)
{
    u32 val = read32(SOC_CORE_BASE_ADDRESS + CORE_CTRL_ADDRESS) & 0x7ff;
    val <<= 21;
    val |= ((addr >= 0x100000) ? 0x100000 : 0) | (addr & 0xfffff);
    return val;
}

/* Target firmware's Copy Engine configuration (ath10k
   pci_target_ce_config_wlan). Only the first
   NUM_TARGET_CE_CONFIG_WLAN entries are pushed to the target for the
   QCA6174 family. CE5 is overridden for QCA6174/QCA9377 exactly like
   ath10k_pci_override_ce_config does. */
static const struct qca_ce_pipe_config kTargetCeConfig[] = {
    /* CE0: host->target HTC control and raw streams */
    { 0, PIPEDIR_OUT,  32,  256, 0, 0 },
    /* CE1: target->host HTT + HTC control */
    { 1, PIPEDIR_IN,   32, 2048, 0, 0 },
    /* CE2: target->host WMI */
    { 2, PIPEDIR_IN,   64, 2048, 0, 0 },
    /* CE3: host->target WMI */
    { 3, PIPEDIR_OUT,  32, 2048, 0, 0 },
    /* CE4: host->target HTT */
    { 4, PIPEDIR_OUT, 256,  256, 0, 0 },
    /* CE5: target->host HTT (HIF->HTT), overridden for QCA6174 */
    { 5, PIPEDIR_OUT,  32, 2048, 0, 0 },
    /* CE6: target autonomous hif_memcpy */
    { 6, PIPEDIR_INOUT, 32, 4096, 0, 0 },
};

/* Map from service/endpoint to Copy Engine. The table is derived from
   the target CE configuration above and is passed to the target at
   startup (ath10k pci_target_service_to_ce_map_wlan). The HTT data
   input is remapped to CE1 for QCA6174/QCA9377. */
static const struct qca_ce_service_to_pipe kServiceToPipeMap[] = {
    { ATH10K_HTC_SVC_ID_WMI_DATA_VO, PIPEDIR_OUT, 3 },
    { ATH10K_HTC_SVC_ID_WMI_DATA_VO, PIPEDIR_IN,  2 },
    { ATH10K_HTC_SVC_ID_WMI_DATA_BK, PIPEDIR_OUT, 3 },
    { ATH10K_HTC_SVC_ID_WMI_DATA_BK, PIPEDIR_IN,  2 },
    { ATH10K_HTC_SVC_ID_WMI_DATA_BE, PIPEDIR_OUT, 3 },
    { ATH10K_HTC_SVC_ID_WMI_DATA_BE, PIPEDIR_IN,  2 },
    { ATH10K_HTC_SVC_ID_WMI_DATA_VI, PIPEDIR_OUT, 3 },
    { ATH10K_HTC_SVC_ID_WMI_DATA_VI, PIPEDIR_IN,  2 },
    { ATH10K_HTC_SVC_ID_WMI_CONTROL, PIPEDIR_OUT, 3 },
    { ATH10K_HTC_SVC_ID_WMI_CONTROL, PIPEDIR_IN,  2 },
    { ATH10K_HTC_SVC_ID_RSVD_CTRL, PIPEDIR_OUT, 0 },
    { ATH10K_HTC_SVC_ID_RSVD_CTRL, PIPEDIR_IN,  1 },
    { ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS, PIPEDIR_OUT, 0 },
    { ATH10K_HTC_SVC_ID_TEST_RAW_STREAMS, PIPEDIR_IN,  1 },
    { ATH10K_HTC_SVC_ID_HTT_DATA_MSG, PIPEDIR_OUT, 4 },
    { ATH10K_HTC_SVC_ID_HTT_DATA_MSG, PIPEDIR_IN,  1 },
    { 0, 0, 0 },
};

/* Map an HTC service id to the Copy Engine pipes used for the uplink
   (host -> target) and the downlink (target -> host) direction. The
   lookup uses the very same table that is downloaded to the target in
   initConfig(), so the host and the target always agree on the pipes
   (ath10k_pci_hif_map_service_to_pipe). */
bool QCAPCIDevice::mapServiceToPipe(u16 serviceId, u8 *ulPipe, u8 *dlPipe) const
{
    bool ulSet = false, dlSet = false;

    for (unsigned i = 0; i < sizeof(kServiceToPipeMap) / sizeof(kServiceToPipeMap[0]); i++) {
        const struct qca_ce_service_to_pipe &entry = kServiceToPipeMap[i];
        if (entry.service_id != serviceId) continue;

        switch (entry.pipedir) {
        case PIPEDIR_NONE:
            break;
        case PIPEDIR_IN:
            *dlPipe = (u8)entry.pipenum;
            dlSet = true;
            break;
        case PIPEDIR_OUT:
            *ulPipe = (u8)entry.pipenum;
            ulSet = true;
            break;
        case PIPEDIR_INOUT:
            *dlPipe = (u8)entry.pipenum;
            *ulPipe = (u8)entry.pipenum;
            dlSet = true;
            ulSet = true;
            break;
        }
    }

    return ulSet && dlSet;
}

/* Download the target-side Copy Engine configuration and the
   service-to-CE map to the target and tell the target that the early
   configuration is complete (ath10k_pci_init_config). Without this the
   target has no valid pipe configuration and refuses to talk to the
   host. */
int QCAPCIDevice::initConfig()
{
    u32 interconnectAddr = QCA_HOST_INTEREST + HI_INTERCONNECT_STATE_OFFSET;
    u32 pcieStateAddr = 0;
    u32 pipeCfgAddr = 0;
    u32 svcToPipeMap = 0;
    u32 configFlags = 0;
    u32 eallocAddr, eallocValue = 0;
    u32 flag2Addr, flag2Value = 0;

    /* The target stores the addresses of its pipe configuration and of
       the service-to-pipe map in the interconnect state structure; the
       host interest area points at that structure. */
    if (fCopyEngine->diagReadMem(interconnectAddr, &pcieStateAddr, sizeof(pcieStateAddr)) != 0) {
        qca_err("Failed to read the interconnect state address");
        return -1;
    }
    setDiagProp("QCAInterconnectAddr", pcieStateAddr);
    if (!pcieStateAddr) {
        qca_err("Invalid interconnect state address");
        return -1;
    }

    if (fCopyEngine->diagReadMem(pcieStateAddr + PCIE_STATE_PIPE_CFG_ADDR_OFFSET,
                                 &pipeCfgAddr, sizeof(pipeCfgAddr)) != 0) {
        qca_err("Failed to read the pipe configuration address");
        return -1;
    }
    setDiagProp("QCAPipeCfgAddr", pipeCfgAddr);
    if (!pipeCfgAddr) {
        qca_err("Invalid pipe configuration address");
        return -1;
    }

    /* Supply the target-side Copy Engine configuration. */
    if (fCopyEngine->diagWriteMem(pipeCfgAddr, kTargetCeConfig,
                                  sizeof(struct qca_ce_pipe_config) * NUM_TARGET_CE_CONFIG_WLAN) != 0) {
        qca_err("Failed to write the pipe configuration");
        return -1;
    }

    if (fCopyEngine->diagReadMem(pcieStateAddr + PCIE_STATE_SVC_TO_PIPE_MAP_OFFSET,
                                 &svcToPipeMap, sizeof(svcToPipeMap)) != 0) {
        qca_err("Failed to read the service-to-pipe map address");
        return -1;
    }
    setDiagProp("QCASvcToPipeAddr", svcToPipeMap);
    if (!svcToPipeMap) {
        qca_err("Invalid service-to-pipe map address");
        return -1;
    }

    /* Supply the service-to-CE map. */
    if (fCopyEngine->diagWriteMem(svcToPipeMap, kServiceToPipeMap,
                                  sizeof(kServiceToPipeMap)) != 0) {
        qca_err("Failed to write the service-to-pipe map");
        return -1;
    }

    /* The target must not enter L1 while the host is driving the
       interconnect. */
    if (fCopyEngine->diagReadMem(pcieStateAddr + PCIE_STATE_CONFIG_FLAGS_OFFSET,
                                 &configFlags, sizeof(configFlags)) != 0) {
        qca_err("Failed to read the PCIe configuration flags");
        return -1;
    }
    configFlags &= ~PCIE_CONFIG_FLAG_ENABLE_L1;
    if (fCopyEngine->diagWriteMem(pcieStateAddr + PCIE_STATE_CONFIG_FLAGS_OFFSET,
                                  &configFlags, sizeof(configFlags)) != 0) {
        qca_err("Failed to write the PCIe configuration flags");
        return -1;
    }

    /* Configure the early allocation: the magic number tells the target
       that the host supports the early allocation and the bank count
       tells it how many IRAM banks it may use. */
    eallocAddr = QCA_HOST_INTEREST + HI_EARLY_ALLOC_OFFSET;
    if (fCopyEngine->diagReadMem(eallocAddr, &eallocValue, sizeof(eallocValue)) != 0) {
        qca_err("Failed to read the early allocation value");
        return -1;
    }
    eallocValue |= ((HI_EARLY_ALLOC_MAGIC << HI_EARLY_ALLOC_MAGIC_SHIFT) &
                    HI_EARLY_ALLOC_MAGIC_MASK);
    eallocValue |= ((QCA9377_IRAM_BANKS << HI_EARLY_ALLOC_IRAM_BANKS_SHIFT) &
                    HI_EARLY_ALLOC_IRAM_BANKS_MASK);
    if (fCopyEngine->diagWriteMem(eallocAddr, &eallocValue, sizeof(eallocValue)) != 0) {
        qca_err("Failed to write the early allocation value");
        return -1;
    }
    setDiagProp("QCAEarlyAlloc", eallocValue);

    /* Tell the target to proceed with the initialization. */
    flag2Addr = QCA_HOST_INTEREST + HI_OPTION_FLAG2_OFFSET;
    if (fCopyEngine->diagReadMem(flag2Addr, &flag2Value, sizeof(flag2Value)) != 0) {
        qca_err("Failed to read the option flag 2 value");
        return -1;
    }
    flag2Value |= HI_OPTION_EARLY_CFG_DONE;
    if (fCopyEngine->diagWriteMem(flag2Addr, &flag2Value, sizeof(flag2Value)) != 0) {
        qca_err("Failed to write the option flag 2 value");
        return -1;
    }
    setDiagProp("QCAOptionFlag2", flag2Value);

    qca_info("Early configuration downloaded to the target");
    return 0;
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
    /* 1. Strong force-wake BEFORE any reset (ath10k_pci_force_wake).
       The target must be fully powered before we touch it. */
    if (!forceWake()) {
        qca_err("Force-wake failed before cold reset");
        return false;
    }

    /* 2. Clear PCI Device Control bit 13 (Disable INTx). macOS never
       programs the MSI capability, so the device is left without any
       interrupt path unless we clear this. */
    clearDisableIntx();

    /* 3. Cold reset (ath10k_pci_cold_reset). */
    setDiagStr("QCAResetSeq", "cold1");
    chipReset();

    /* 4. Brief settle: let the ROM initialize hardware (including the
       Copy Engine diagnostic window). ath10k waits for INITIALIZED here,
       but on this hardware the ROM waits for initConfig first. */
    setDiagStr("QCAStartStage", "fw-settle");
    IOSleep(200);  // 200 ms for ROM to init CE hardware

    /* 5. Download the target-side Copy Engine configuration and the
       service-to-CE map, then release the target from its early
       configuration wait (ath10k_pci_init_config). This step uses the
       Copy Engine 7 diagnostic window, which the ROM initializes early. */
    setDiagStr("QCAStartStage", "fw-init-config");
    if (initConfig() != 0) {
        qca_warn("Early configuration download failed; continuing");
        setDiagStr("QCAInitConfig", "failed");
    } else {
        setDiagStr("QCAInitConfig", "ok");
    }

    /* 6. Now wait for the ROM to signal INITIALIZED. The target should
       advance to INITIALIZED after the host sets EARLY_CFG_DONE. */
    setDiagStr("QCAStartStage", "fw-wait-init");
    if (waitForTargetInit(3000) != 0) {
        qca_err("Target did not reach INITIALIZED after initConfig");
        return false;
    }
    setDiagStr("QCAIndAfterConfig", "yes");

    /* 7. Enable PCIe interrupts on the SoC core and assert CPU_INTR_MASK
       (ath10k_pci_wake_target_cpu). Now that the target is initialized,
       it can handle interrupts. */
    enablePcieInterrupts();
    setCoreCtrlIntr(true);

    /* 8. Query ROM target info — authoritative liveness probe. */
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

    /* 9. Enable the PLL clock before bulk transfers
       (ath10k_hw_qca6174_enable_pll_clock; failure is non-fatal). */
    setDiagStr("QCAStartStage", "fw-pll");
    if (enablePllClock() != 0) {
        qca_info("PLL clock config failed; continuing at ROM clock");
    }

    /* 10. Configure the target's shared host-interest settings before
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
