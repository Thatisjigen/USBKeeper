//
//  The byte-level part of USBDiskKeeper: finding, in IOUSBMassStorageDriverNub::setPowerState, the calls to
//  IOUSBMassStorageInterfaceNub::ClampPowerState. No kernel dependencies, so tests/ runs the same code on the bytes of
//  the kernel collection on disk.
//
#ifndef callsite_hpp
#define callsite_hpp

#include <stddef.h>
#include <stdint.h>

namespace CallSite {

typedef int32_t __attribute__((aligned(1))) rel32_t;

static constexpr size_t NotFound = ~static_cast<size_t>(0);

struct ClampCalls {
	size_t count {0};           // near calls to ClampPowerState in the scanned bytes
	size_t on {NotFound};       // offset of the call made with state 1
	size_t off {NotFound};      // offset of the call made with state 0
	bool asExpected() const { return count == 2 && on != NotFound && off != NotFound; }
};

//  Where a near call at this address lands.
static inline uint64_t callTarget(const uint8_t *call, uint64_t address) {
	return address + 5 + static_cast<uint64_t>(static_cast<int64_t>(*reinterpret_cast<const rel32_t *>(call + 1)));
}

//  code: the first `span` bytes of setPowerState, which runs at `address`. A byte 0xE8 counts as a call to
//  ClampPowerState only when its displacement lands exactly on `clamp`. The two calls are told apart by the
//  instruction that loads the state argument right before them.
static inline ClampCalls findClampCalls(const uint8_t *code, size_t span, uint64_t address, uint64_t clamp) {
	ClampCalls found;
	for (size_t i = 3; i + 5 <= span; i++) {
		if (code[i] != 0xE8 || callTarget(code + i, address + i) != clamp)
			continue;
		found.count++;
		if (code[i - 3] == 0x6A && code[i - 2] == 0x01 && code[i - 1] == 0x5E)      // pushq $1; popq %rsi
			found.on = i;
		else if (code[i - 2] == 0x31 && code[i - 1] == 0xF6)                        // xorl %esi, %esi
			found.off = i;
	}
	return found;
}

//  The displacement that makes a near call at `address` land on `target`; false when it does not fit in 32 bits.
static inline bool nearDisplacement(uint64_t address, uint64_t target, int32_t &disp) {
	const int64_t wide = static_cast<int64_t>(target - (address + 5));
	disp = static_cast<int32_t>(wide);
	return wide == disp;
}

}

#endif /* callsite_hpp */
