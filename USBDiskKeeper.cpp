//
//  USBDiskKeeper: a USB disk survives a port that drops its device right after wake.
//
//  Apple's IOUSBMassStorageDriver parks its driver nub under IOUSBMassStorageResource for the whole sleep. At wake
//  the driver nub asks the interface nub it had before sleep to power on,
//  IOUSBMassStorageInterfaceNub::ClampPowerState(1), and then takes one of two paths:
//    - the old interface nub is already terminated (kIOReturnNoDevice): the driver nub stays parked, waits 1500 ms
//      for the device to be enumerated again and re-attaches to the new interface nub. Same disk, volumes stay
//      mounted, and the log says "re-instantiated during wake up";
//    - the old interface nub powers on (success): the driver nub re-attaches to it at once. When the port declares
//      the device lost a moment later, the driver nub is a client of the interface nub being terminated, and the
//      whole disk stack goes with it unless a reset happens to be past its 300 ms pause.
//  Some boards drop the device on a port 0.1-0.2 s after the xHCI controller is back and re-attach it about half a
//  second later. The port declares the loss only after a 100 ms debounce, so which path a wake takes is a race, and
//  the second one ends with the disk ejected by force.
//
//  The fix: after a successful ClampPowerState(1) the driver nub is held parked for a settle window. A parked nub
//  is not a client of the old interface nub, so that nub's termination cannot reach it. If the old interface nub
//  turns inactive within the window the call returns kIOReturnNoDevice and the driver nub takes Apple's first path.
//  If it stays active the call returns success, as before. The wait ends as soon as the loss is declared, so a port
//  that drops its device costs nothing more than Apple's first path already does.
//
//  How it is hooked: ClampPowerState is not virtual and has two callers, both in
//  IOUSBMassStorageDriverNub::setPowerState. The call made with state 1 is redirected by rewriting the displacement
//  of that one call instruction; the function itself stays as it is and is called at its own address. Lilu solves
//  the symbols and opens kernel text for the write, but no trampoline is built: Lilu 1.7.2 hands out trampolines from
//  one 4096-byte pool shared by every plugin, and when the pool is full it reports success and routes nothing.
//  Nothing is written unless setPowerState holds exactly the two expected calls.
//
//  Boot-args: -udkoff, -udkdbg, -udkbeta; udksettle=<ms> (default 2000, 0 = only refuse a nub that is already
//  dead); udkport=<locationID> (only that port; default every port).
//
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_util.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSNumber.h>
#include <mach/mach_time.h>

#include "callsite.hpp"

static const char *kextPath[] {"/System/Library/Extensions/IOUSBMassStorageDriver.kext/Contents/MacOS/IOUSBMassStorageDriver"};
// Loaded: the kext is in the boot collection and is usually loaded before Lilu's patcher is.
static KernelPatcher::KextInfo kextMassStorage {"com.apple.iokit.IOUSBMassStorageDriver", kextPath, arrsize(kextPath), {true}, {}, KernelPatcher::KextInfo::Unloaded};

static const char symClamp[] = "__ZN28IOUSBMassStorageInterfaceNub15ClampPowerStateEm";
static const char symSetPowerState[] = "__ZN25IOUSBMassStorageDriverNub13setPowerStateEmP9IOService";

static constexpr size_t ScanSpan = 0x400;            // IOUSBMassStorageDriverNub::setPowerState is 0x3d8 bytes in macOS 26.7.2
static constexpr uint32_t SettleDefaultMs = 2000;
static constexpr uint32_t SettleMaxMs = 10000;       // Apple's own poll in the same call gives up after 15 s
static constexpr uint32_t PollMs = 10;               // the step of Apple's poll

static uint32_t settleMs = SettleDefaultMs;
static uint32_t onlyPort = 0;
static mach_vm_address_t orgClamp = 0;

static uint32_t msSince(uint64_t start) {
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
	return static_cast<uint32_t>(ns / 1000000);
}

static uint32_t portOf(IOService *nub) {
	uint32_t port = 0;
	auto obj = nub->copyProperty("locationID");
	if (auto num = OSDynamicCast(OSNumber, obj))
		port = num->unsigned32BitValue();
	OSSafeReleaseNULL(obj);
	return port;
}

//  Called in place of IOUSBMassStorageInterfaceNub::ClampPowerState(1) from IOUSBMassStorageDriverNub::setPowerState,
//  on the power management thread of the driver nub. The driver nub holds a reference to the interface nub for the
//  whole call, and Apple's code already sleeps here in 10 ms steps.
static IOReturn wrapClampPowerState(IOService *nub, unsigned long state) {
	const uint64_t called = mach_absolute_time();
	const IOReturn ret = FunctionCast(wrapClampPowerState, orgClamp)(nub, state);
	if (state != 1)
		return ret;

	const uint32_t clampMs = msSince(called);
	const uint32_t port = portOf(nub);
	if (ret != kIOReturnSuccess) {
		// Apple's first path: the device is gone, or its interface nub never powered on.
		SYSLOG("udk", "port 0x%08x: the old interface nub did not power on (0x%x after %u ms), the disk waits for the new one", port, ret, clampMs);
		return ret;
	}

	// The original returns success without looking when the nub powered on after it was terminated. Re-attaching to
	// that nub fails in open() and the stack is terminated.
	if (nub->isInactive()) {
		SYSLOG("udk", "port 0x%08x: the old interface nub is already terminated, the disk waits for the new one", port);
		return kIOReturnNoDevice;
	}

	if (onlyPort != 0 && port != onlyPort) {
		DBGLOG("udk", "port 0x%08x: not the port asked for with udkport, left alone", port);
		return ret;
	}

	const uint64_t powered = mach_absolute_time();
	while (msSince(powered) < settleMs) {
		IOSleep(PollMs);
		if (nub->isInactive()) {
			SYSLOG("udk", "port 0x%08x: the old interface nub powered on after %u ms and was terminated %u ms later, the disk waits for the new one",
				port, clampMs, msSince(powered));
			return kIOReturnNoDevice;
		}
	}

	SYSLOG("udk", "port 0x%08x: the device kept its connection for %u ms after power on, the disk re-attaches to it", port, msSince(powered));
	return ret;
}

static void onMassStorageLoad(void *, KernelPatcher &patcher, size_t id, mach_vm_address_t slide, size_t size) {
	if (id != kextMassStorage.loadIndex)
		return;

	// The ranged lookup returns 0, without a panic, for a symbol that is missing or outside the kext.
	const auto clamp = patcher.solveSymbol(id, symClamp, slide, size);
	const auto setPowerState = patcher.solveSymbol(id, symSetPowerState, slide, size);
	patcher.clearError();
	if (!clamp || !setPowerState) {
		SYSLOG("udk", "not installed: symbols not found (ClampPowerState %d, setPowerState %d)", clamp != 0, setPowerState != 0);
		return;
	}

	size_t span = ScanSpan;
	if (setPowerState + span > slide + size)
		span = static_cast<size_t>(slide + size - setPowerState);

	const auto calls = CallSite::findClampCalls(reinterpret_cast<const uint8_t *>(setPowerState), span, setPowerState, clamp);
	if (!calls.asExpected()) {
		SYSLOG("udk", "not installed: setPowerState has %lu calls to ClampPowerState (state 1 %d, state 0 %d), expected one of each",
			calls.count, calls.on != CallSite::NotFound, calls.off != CallSite::NotFound);
		return;
	}

	const mach_vm_address_t callOn = setPowerState + calls.on;
	const auto target = reinterpret_cast<mach_vm_address_t>(wrapClampPowerState);
	int32_t disp = 0;
	if (!CallSite::nearDisplacement(callOn, target, disp)) {
		SYSLOG("udk", "not installed: this kext is out of reach of a near call from IOUSBMassStorageDriver");
		return;
	}

	// The original must be callable before the call can land here.
	orgClamp = clamp;
	if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
		SYSLOG("udk", "not installed: kernel text cannot be written");
		return;
	}
	*reinterpret_cast<CallSite::rel32_t *>(callOn + 1) = disp;
	MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

	if (CallSite::callTarget(reinterpret_cast<const uint8_t *>(callOn), callOn) != target) {
		SYSLOG("udk", "not installed: the call at setPowerState+0x%lx did not take the new target", calls.on);
		return;
	}
	SYSLOG("udk", "installed: ClampPowerState(1) is called through this kext (call at setPowerState+0x%lx), settle %u ms, port 0x%x",
		calls.on, settleMs, onlyPort);
	DBGLOG("udk", "ClampPowerState 0x%llx, setPowerState 0x%llx, other call at +0x%lx, wrapper 0x%llx", clamp, setPowerState,
		calls.off, target);
}

static void pluginStart() {
	uint32_t value = 0;
	if (lilu_get_boot_args("udksettle", &value, sizeof(value)))
		settleMs = value > SettleMaxMs ? SettleMaxMs : value;
	if (lilu_get_boot_args("udkport", &value, sizeof(value)))
		onlyPort = value;

	const auto error = lilu.onKextLoad(&kextMassStorage, 1, onMassStorageLoad);
	if (error != LiluAPI::Error::NoError)
		SYSLOG("udk", "not installed: Lilu refused the kext load request (%d)", static_cast<int>(error));
}

static const char *bootargOff[] {"-udkoff"};
static const char *bootargDebug[] {"-udkdbg"};
static const char *bootargBeta[] {"-udkbeta"};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::Tahoe,   // the two call sites were read in macOS 26.7.2 (25G332)
	KernelVersion::Tahoe,
	pluginStart
};
