#!/usr/bin/env python3
"""What USBDiskKeeper relies on in Apple's USB disk driver, read from the kernel collection on disk.

  python3 -I tools/check_collection.py [collection] [--dump file]
      collection   default /System/Library/KernelCollections/BootKernelExtensions.kc
      --dump file  also writes the bytes of setPowerState that the kext scans, for tests/run.sh

Solves the two symbols the way Lilu 1.7.2 does for a kext of the boot collection (kern_mach.cpp, kcGetRunningAddresses and
solveSymbol: the entry's own LC_SYMTAB inside the shared __LINKEDIT, first non-stab entry with that exact name), then looks,
in IOUSBMassStorageDriverNub::setPowerState, for the two calls to IOUSBMassStorageInterfaceNub::ClampPowerState and for the
two places that arm the 1500 ms re-enumeration timer (changed only with udktimer=). Prints what
it finds and exits 1 if anything differs from what the kext expects; the kext makes the same checks on the running kernel and
installs nothing when they fail. Lilu reads the collection in memory, where xnu has already slid the symbol values; the
addresses printed here are the unslid ones.
"""
import hashlib, struct, sys

args = sys.argv[1:]
DUMP = None
if '--dump' in args:
    i = args.index('--dump'); DUMP = args[i + 1]; del args[i:i + 2]
KC = args[0] if args else '/System/Library/KernelCollections/BootKernelExtensions.kc'
ENTRY = 'com.apple.iokit.IOUSBMassStorageDriver'
CLAMP = '__ZN28IOUSBMassStorageInterfaceNub15ClampPowerStateEm'
POWER = '__ZN25IOUSBMassStorageDriverNub13setPowerStateEmP9IOService'
SCAN = 0x400                                                     # the kext scans this many bytes of setPowerState
TIMER = bytes.fromhex('bedc050000ff90d0010000')                  # movl $1500, %esi; callq *0x1d0(%rax)
LC_SEGMENT_64, LC_SYMTAB, LC_UUID, LC_FILESET_ENTRY = 0x19, 0x2, 0x1b, 0x80000035
kc = open(KC, 'rb').read()
bad = []


def commands(base):
    magic, _, _, _, ncmds, _ = struct.unpack_from('<6I', kc, base)
    assert magic == 0xfeedfacf, hex(magic)
    off = base + 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', kc, off)
        yield off, cmd, size
        off += size


def segments(base):
    out = {}
    for off, cmd, size in commands(base):
        if cmd == LC_SEGMENT_64:
            name = kc[off + 8:off + 24].rstrip(b'\0').decode()
            out[name] = struct.unpack_from('<4Q', kc, off + 24)   # vmaddr, vmsize, fileoff, filesize
    return out


entry = None
for off, cmd, size in commands(0):
    if cmd == LC_FILESET_ENTRY:
        vmaddr, fileoff, name_off = struct.unpack_from('<QQI', kc, off + 8)
        if kc[off + name_off:kc.index(b'\0', off + name_off)].decode() == ENTRY:
            entry = (vmaddr, fileoff)
assert entry, f'{ENTRY} is not in {KC}'
segs = segments(entry[1])
text = segs['__TEXT_EXEC'] if '__TEXT_EXEC' in segs else segs['__TEXT']
symtab = uuid = None
for off, cmd, size in commands(entry[1]):
    if cmd == LC_SYMTAB:
        symtab = struct.unpack_from('<4I', kc, off + 8)           # symoff, nsyms, stroff, strsize
    elif cmd == LC_UUID:
        u = kc[off + 8:off + 24].hex().upper()
        uuid = f'{u[:8]}-{u[8:12]}-{u[12:16]}-{u[16:20]}-{u[20:]}'
link = segs['__LINKEDIT']
print(f'{KC}\n  sha256 {hashlib.sha256(kc).hexdigest()[:16]}, {len(kc)} bytes')
print(f'{ENTRY}: header at {entry[0]:#x}, UUID {uuid}')
print(f'  __LINKEDIT vmaddr {link[0]:#x} fileoff {link[2]:#x} size {link[1]:#x}; LC_SYMTAB symoff {symtab[0]:#x} nsyms {symtab[1]} stroff {symtab[2]:#x}')
assert link[2] <= symtab[0] and symtab[0] + 16 * symtab[1] <= link[2] + link[3], 'the symbol table is outside __LINKEDIT: Lilu would refuse it'


def solve(name):
    """Lilu's MachInfo::solveSymbol: linear scan, stabs skipped, exact name, the first hit wins."""
    hits = []
    for i in range(symtab[1]):
        strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', kc, symtab[0] + 16 * i)
        if n_type & 0xe0:
            continue
        s = symtab[2] + strx
        if kc[s:s + len(name) + 1] == name.encode() + b'\0':
            hits.append((n_value, n_type, n_sect))
    return hits


def vm2off(vm):
    for vmaddr, vmsize, fileoff, filesize in segs.values():
        if vmaddr <= vm < vmaddr + filesize:
            return fileoff + (vm - vmaddr)
    raise SystemExit(f'{vm:#x} is not in a segment of the entry')


addr = {}
for name in (CLAMP, POWER):
    hits = solve(name)
    ok = len(hits) == 1 and hits[0][1] & 0x0e == 0x0e and text[0] <= hits[0][0] < text[0] + text[1]
    print(f'  {name}: ' + (', '.join(f'{v:#x} type {t:#x} sect {s}' for v, t, s in hits) or 'NOT FOUND') + ('' if ok else '   <-- not one defined text symbol'))
    if not ok:
        bad.append(name)
    addr[name] = hits[0][0] if hits else 0
assert not bad, 'symbols missing'
clamp, power = addr[CLAMP], addr[POWER]
print(f'ClampPowerState, first 16 bytes: {kc[vm2off(clamp):vm2off(clamp) + 16].hex(" ")}')
body = kc[vm2off(power):vm2off(power) + SCAN]
calls = [i for i in range(len(body) - 5) if body[i] == 0xe8 and power + i + 5 + struct.unpack_from('<i', body, i + 1)[0] == clamp]
print(f'setPowerState, first {SCAN:#x} bytes: sha256 {hashlib.sha256(body).hexdigest()[:16]}; calls to ClampPowerState at ' + ', '.join(f'+{i:#x} ({power + i:#x})' for i in calls))
on = [i for i in calls if body[i - 3:i] == bytes.fromhex('6a015e')]          # pushq $1; popq %rsi
off = [i for i in calls if body[i - 2:i] == bytes.fromhex('31f6')]            # xorl %esi, %esi
print(f'  with state 1 (6a 01 5e before the call): {[hex(i) for i in on]}; with state 0 (31 f6 before): {[hex(i) for i in off]}')
if len(calls) != 2 or len(on) != 1 or len(off) != 1:
    bad.append('call sites')
whole = kc[vm2off(text[0]):vm2off(text[0]) + text[3]]
timers = [i for i in range(len(body) - len(TIMER) + 1) if body[i:i + len(TIMER)] == TIMER]
everywhere = [text[0] + i for i in range(len(whole) - len(TIMER) + 1) if whole[i:i + len(TIMER)] == TIMER]
print(f'  setTimeoutMS(1500) ({TIMER.hex(" ")}): in the scanned bytes at {[hex(i) for i in timers]}; in the whole text of the entry at {[hex(a) for a in everywhere]}')
if len(timers) != 2 or everywhere != [power + i for i in timers]:
    bad.append('timer immediates (udktimer= would be refused)')
# every caller of ClampPowerState in the entry: a direct call or jump from anywhere else would bypass the redirected call
callers = [text[0] + i for i in range(len(whole) - 5) if whole[i] in (0xe8, 0xe9) and text[0] + i + 5 + struct.unpack_from('<i', whole, i + 1)[0] == clamp]
print(f'  direct calls or jumps to ClampPowerState in the whole text: {[hex(a) for a in callers]}')
if callers != [power + i for i in calls]:
    bad.append('other callers')
if DUMP:
    open(DUMP, 'wb').write(body)
    print(f'DUMP {DUMP} {power:#x} {clamp:#x} {on[0] if on else 0:#x} {off[0] if off else 0:#x} ' + ' '.join(f'{i + 1:#x}' for i in timers))
print('RESULT:', 'as the kext expects' if not bad else 'DIFFERENT: ' + ', '.join(bad))
sys.exit(1 if bad else 0)
