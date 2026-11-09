# USBDiskKeeper

A [Lilu](https://github.com/acidanthera/Lilu) plugin that keeps a USB disk attached when its port drops the device right after wake.

## The problem

Some boards cut a USB port for a moment when they resume from S3: the device disappears 0.1 to 0.2 s after the xHCI controller is back and returns about half a second later. macOS enumerates it again. For a disk, Apple's `IOUSBMassStorageDriver` can survive this and keep the same disk with its volumes mounted, but only when it learns of the loss early enough:

- During sleep the driver nub (`IOUSBMassStorageDriverNub`) is parked under `IOUSBMassStorageResource`. At wake it asks the interface nub it had before sleep to power on, with `IOUSBMassStorageInterfaceNub::ClampPowerState(1)`.
- If that interface nub is already terminated, the call fails, the driver nub stays parked, waits 1500 ms for the device to come back and re-attaches to the new interface nub. The log says `re-instantiated during wake up`. Nothing is unmounted.
- If the interface nub powers on, the driver nub re-attaches to it at once. When the port declares the loss a moment later (it debounces for 100 ms first), the driver nub is a client of an interface nub that is being terminated, and the whole disk stack is destroyed with it. The volume is ejected by force; a new disk appears a second later.

Which of the two happens is a race between the board and the port's debounce. On the machine this was written for, about one wake in three ended in a forced eject.

## The fix

After a successful `ClampPowerState(1)` the plugin holds the driver nub parked for a settle window and watches the old interface nub:

- it turns inactive within the window: the call returns `kIOReturnNoDevice`, and the driver nub takes Apple's own first path. The wait ends as soon as the loss is declared, so a port that drops its device waits no longer than that path already does;
- it stays active for the whole window: the call returns success, as before. A disk on a port that does not drop comes back that much later after a wake.

It also refuses an interface nub that is already terminated when the call returns success. Apple's function has an early exit that does not look, and re-attaching to such a nub ends in `PowerOnTerminate`.

A disk really unplugged during sleep is handled as before: the driver nub waits its 1500 ms and the stack is torn down.

## How it hooks

`ClampPowerState` is not virtual and has exactly two callers, both in `IOUSBMassStorageDriverNub::setPowerState`. The plugin rewrites the displacement of the one `call` made with state 1, so that it lands in the plugin, and calls the original at its own address. The function itself is not modified and no trampoline is built: Lilu 1.7.2 hands out trampolines from a single 4096-byte pool shared by all plugins, and once the pool is full it reports success and routes nothing.

Lilu is used to be told when the kext is loaded, to solve the two symbols in the running kernel collection and to open kernel text for the four-byte write. Nothing is written unless `setPowerState` holds exactly one call with state 1 and one with state 0, and the plugin is within reach of a near call.

## Requirements

- macOS 26 (Tahoe). The call sites were read in 26.7.2 (25G332); `-udkbeta` lets it try on a newer kernel, where it still installs nothing if the code no longer looks the same.
- Lilu 1.7.2 or newer, loaded before this kext.

## Boot-args

| Argument | Meaning |
|---|---|
| `-udkoff` | disables the plugin |
| `-udkdbg` | more lines in a Debug build |
| `-udkbeta` | load on a kernel newer than Tahoe |
| `udksettle=<ms>` | settle window, default 2000, at most 10000. `0` keeps only the check for an already terminated nub |
| `udkport=<locationID>` | act only on the disk at this port, for example `udkport=0x14400000`. Default: every port |

## What it reports

Its own entry in the registry holds the result, readable at any time and without root:

```bash
ioreg -r -c USBDiskKeeper -l -w0
```

| Property | Meaning |
|---|---|
| `Hook` | `installed`, or `not installed: ...` with the reason, or `waiting for IOUSBMassStorageDriver to load` |
| `SettleMs`, `Port` | the settle window and the port filter in force (`0` = every port) |
| `Wakes` | how many times a disk went through the hook |
| `Parked` | the old interface nub powered on and was terminated within the window: the forced eject this plugin prevents |
| `Gone` | the old interface nub did not power on: Apple's own path, nothing to do |
| `Dead` | the old interface nub was already terminated when Apple's call returned success |
| `Kept` | no loss within the window: the disk re-attached to the device it had |
| `Skipped` | a disk on another port than the one given with `udkport` |
| `Events` | the last eight, oldest first: number, outcome, port, `clamp` (ms spent in Apple's call), `settle` (ms held after it), `at` (wall clock, seconds since 1970) |

A wake that kept the disk shows `Parked` or `Gone` here, and in the system log Apple's own `re-instantiated during wake up`.

## What it logs

One line when `IOUSBMassStorageDriver` is loaded (`installed: ...` or `not installed: ...` with the reason) and one line for every disk at every wake:

- `the old interface nub did not power on (...), the disk waits for the new one`: Apple's own first path, nothing done;
- `the old interface nub powered on after N ms and was terminated M ms later, the disk waits for the new one`: the case this plugin exists for;
- `the old interface nub is already terminated, the disk waits for the new one`;
- `the device kept its connection for N ms after power on, the disk re-attaches to it`: no loss on this wake.

The lines go through Lilu's logger. With a Debug build of Lilu the one written at boot is in Lilu's dump file (`liludump=N`); all of them are in the kernel message buffer (`sudo dmesg | grep USBDiskKeeper`). A kext injected by a boot loader does not reach the unified log, so `log show` does not have them.

## Checking a macOS build before installing

```bash
python3 -I tools/check_collection.py
```

reads the boot kernel collection on disk the way Lilu reads it in memory and says whether the two symbols and the two calls are where the plugin expects them.

```bash
bash tests/run.sh
```

runs the plugin's own call-site code on those bytes, and on altered copies of them, outside the kernel.

## Building

```bash
bash build.sh Release
```

```bash
bash build.sh Debug
```

Only the Command Line Tools are needed. The script uses [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) and the plugin SDK inside `Lilu-1.7.2-DEBUG.zip`; `MKSDK=` and `LILU_ZIP=` point it at local copies. Lilu's `plugin_start.cpp` (BSD 3-Clause, vit9696) is compiled into the kext.

## Limits

- The settle window is a guess about the board. If the loss is declared later than the window, the wake goes as it did without the plugin.
- Apple's 1500 ms wait for the device to come back is unchanged. If the device takes longer, the disk is torn down (`not found during wake up`).
- A loss while the system is awake is not covered: the driver nub is attached then, and only a reset in progress saves it.
- The plugin acts where `IOUSBMassStorageDriver` is the driver: Bulk-Only and UAS disks and card readers. Other USB devices that drop at wake are not its business.
