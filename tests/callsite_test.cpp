//
//  Host test for callsite.hpp: the code the kext runs on kernel memory, run here on the bytes of
//  IOUSBMassStorageDriverNub::setPowerState taken from the kernel collection on disk, and on altered copies of them.
//
//  callsite_test <bytes> <setPowerState address> <ClampPowerState address> <offset of the state 1 call>
//                <offset of the state 0 call> <offset of the first timer immediate> <offset of the second>
//  The offsets come from tools/check_collection.py, which finds them with its own code.
//
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "../callsite.hpp"

static int failures = 0;

#define CHECK(cond) do { if (!(cond)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #cond); } } while (0)

int main(int argc, char **argv) {
	if (argc != 8) {
		fprintf(stderr, "usage: callsite_test <bytes> <setPowerState> <ClampPowerState> <state 1 call> <state 0 call> <timer 1> <timer 2>\n");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	std::vector<uint8_t> code(0x1000);
	code.resize(fread(code.data(), 1, code.size(), f));
	fclose(f);
	const uint64_t address = strtoull(argv[2], nullptr, 16), clamp = strtoull(argv[3], nullptr, 16);
	const size_t wantOn = strtoull(argv[4], nullptr, 16), wantOff = strtoull(argv[5], nullptr, 16);
	const size_t wantTimer[2] {strtoull(argv[6], nullptr, 16), strtoull(argv[7], nullptr, 16)};

	// The real bytes: two calls, one of each kind, where the independent scan found them.
	const auto calls = CallSite::findClampCalls(code.data(), code.size(), address, clamp);
	CHECK(calls.asExpected());
	CHECK(calls.count == 2);
	CHECK(calls.on == wantOn);
	CHECK(calls.off == wantOff);
	printf("real bytes: %zu calls, state 1 at +0x%zx, state 0 at +0x%zx\n", calls.count, calls.on, calls.off);
	if (!calls.asExpected())
		return 1;

	// Redirecting the state 1 call: four bytes change, the call lands on the new target, the other call is untouched.
	// Distances are from the end of the call instruction, up to the two ends of what a near call reaches.
	const uint64_t next = address + calls.on + 5;
	for (const int64_t distance : {0x1E00000LL, -0x1E00000LL, 0x7FFFFFFFLL, -0x80000000LL, 0x10LL}) {
		const uint64_t target = next + static_cast<uint64_t>(distance);
		int32_t disp = 0;
		CHECK(CallSite::nearDisplacement(address + calls.on, target, disp));
		auto patched = code;
		*reinterpret_cast<CallSite::rel32_t *>(patched.data() + calls.on + 1) = disp;
		CHECK(CallSite::callTarget(patched.data() + calls.on, address + calls.on) == target);
		CHECK(CallSite::callTarget(patched.data() + calls.off, address + calls.off) == clamp);
		size_t changed = 0;
		for (size_t i = 0; i < code.size(); i++)
			if (patched[i] != code[i]) {
				changed++;
				CHECK(i > calls.on && i <= calls.on + 4);
			}
		CHECK(changed >= 1 && changed <= 4);
		// A second look at patched code finds one call only and must refuse.
		CHECK(!CallSite::findClampCalls(patched.data(), patched.size(), address, clamp).asExpected());
	}

	// One byte past either end, and far away: refused.
	int32_t disp = 0;
	CHECK(!CallSite::nearDisplacement(address + calls.on, next + 0x80000000ULL, disp));
	CHECK(!CallSite::nearDisplacement(address + calls.on, next - 0x80000001ULL, disp));
	CHECK(!CallSite::nearDisplacement(address + calls.on, 0x1000, disp));

	// Altered layouts are refused.
	{
		auto other = code;                       // the state is loaded some other way before the state 1 call
		other[calls.on - 3] = 0x90;
		const auto c = CallSite::findClampCalls(other.data(), other.size(), address, clamp);
		CHECK(c.count == 2 && !c.asExpected());
	}
	{
		auto other = code;                       // the state 0 call is gone
		memset(other.data() + calls.off, 0x90, 5);
		const auto c = CallSite::findClampCalls(other.data(), other.size(), address, clamp);
		CHECK(c.count == 1 && !c.asExpected());
	}
	{
		auto other = code;                       // a third call to ClampPowerState appears
		const size_t at = 0x40;
		other[at] = 0xE8;
		int32_t d = 0;
		CHECK(CallSite::nearDisplacement(address + at, clamp, d));
		*reinterpret_cast<CallSite::rel32_t *>(other.data() + at + 1) = d;
		const auto c = CallSite::findClampCalls(other.data(), other.size(), address, clamp);
		CHECK(c.count == 3 && !c.asExpected());
	}
	{
		const auto c = CallSite::findClampCalls(code.data(), code.size(), address, clamp + 1);   // another function
		CHECK(c.count == 0 && !c.asExpected());
	}
	{
		const auto c = CallSite::findClampCalls(code.data(), calls.on + 4, address, clamp);       // cut inside the call
		CHECK(c.count == 1 && !c.asExpected());
	}
	{
		const auto c = CallSite::findClampCalls(code.data(), 0, address, clamp);
		CHECK(c.count == 0 && !c.asExpected());
	}

	// The re-enumeration timer: two places arm it with 1500 ms.
	const auto loads = CallSite::findTimerLoads(code.data(), code.size());
	CHECK(loads.asExpected());
	CHECK(loads.at[0] == wantTimer[0] && loads.at[1] == wantTimer[1]);
	printf("real bytes: timer armed in %zu places, immediates at +0x%zx and +0x%zx\n", loads.count, loads.at[0], loads.at[1]);
	if (loads.asExpected()) {
		for (const auto at : loads.at)
			CHECK(*reinterpret_cast<const CallSite::rel32_t *>(code.data() + at) == static_cast<int32_t>(CallSite::TimerDefaultMs));
		// After the change both read the new value, nothing else moved, and a second look finds none left to change.
		auto patched = code;
		for (const auto at : loads.at)
			*reinterpret_cast<CallSite::rel32_t *>(patched.data() + at) = 5000;
		size_t changed = 0;
		for (size_t i = 0; i < code.size(); i++)
			if (patched[i] != code[i]) {
				changed++;
				CHECK((i >= loads.at[0] && i < loads.at[0] + 4) || (i >= loads.at[1] && i < loads.at[1] + 4));
			}
		CHECK(changed == 4);                     // 0x5DC -> 0x1388: the two low bytes of each immediate
		CHECK(CallSite::findTimerLoads(patched.data(), patched.size()).count == 0);
		CHECK(CallSite::findClampCalls(patched.data(), patched.size(), address, clamp).asExpected());
		// One place only, or three: refused.
		auto one = code;
		one[loads.at[0]] ^= 0xFF;
		CHECK(CallSite::findTimerLoads(one.data(), one.size()).count == 1);
		CHECK(!CallSite::findTimerLoads(one.data(), one.size()).asExpected());
		auto three = code;
		memcpy(three.data() + 0x20, code.data() + loads.at[0] - 1, 11);
		CHECK(CallSite::findTimerLoads(three.data(), three.size()).count == 3);
		CHECK(!CallSite::findTimerLoads(three.data(), three.size()).asExpected());
		CHECK(CallSite::findTimerLoads(code.data(), loads.at[1] + 9).count == 1);     // cut inside the second pattern
	}
	CHECK(CallSite::findTimerLoads(code.data(), 0).count == 0);

	printf("%s\n", failures ? "FAILED" : "all checks passed");
	return failures ? 1 : 0;
}
