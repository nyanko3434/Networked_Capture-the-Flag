// shared/bytebuffer tests (README §5.2, §10: "Serialization round-trip",
// "Malformed-packet fuzzing"). Three cases named in implementation_guide.md
// §1.3's acceptance criteria: round-trip, truncation, overflow.

#include "bytebuffer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using namespace ctf;

namespace {

void test_round_trip() {
    // Write one value of every supported type, including boundary values
    // (0, max, and negative for i16), read them back in the same order.
    uint8_t buf[64];
    ByteWriter w(buf, sizeof(buf));

    const uint8_t u8_zero = 0, u8_max = 0xFF;
    const uint16_t u16_zero = 0, u16_max = 0xFFFF;
    const uint32_t u32_zero = 0, u32_max = 0xFFFFFFFFu;
    const int16_t i16_zero = 0, i16_max = 32767, i16_min = -32768, i16_neg = -1234;

    w.u8(u8_zero);
    w.u8(u8_max);
    w.u16(u16_zero);
    w.u16(u16_max);
    w.u32(u32_zero);
    w.u32(u32_max);
    w.i16(i16_zero);
    w.i16(i16_max);
    w.i16(i16_min);
    w.i16(i16_neg);

    assert(w.ok());

    ByteReader r(buf, w.size());
    assert(r.u8() == u8_zero);
    assert(r.u8() == u8_max);
    assert(r.u16() == u16_zero);
    assert(r.u16() == u16_max);
    assert(r.u32() == u32_zero);
    assert(r.u32() == u32_max);
    assert(r.i16() == i16_zero);
    assert(r.i16() == i16_max);
    assert(r.i16() == i16_min);
    assert(r.i16() == i16_neg);
    assert(r.ok());

    printf("test_round_trip: OK\n");
}

void test_truncation() {
    // A buffer one byte shorter than a full valid message: every read past
    // the truncation point returns 0, ok() becomes false, no crash / OOB
    // (verified under ASan by the CMake build config).
    uint8_t full[64];
    ByteWriter w(full, sizeof(full));
    w.u32(0xDEADBEEF);
    w.u16(0xBEEF);
    w.u8(0x42);
    assert(w.ok());
    size_t full_size = w.size(); // 7 bytes

    // One byte short: the u8 at the end can't be read.
    ByteReader r(full, full_size - 1);
    assert(r.u32() == 0xDEADBEEF); // still fully buffered
    assert(r.ok());
    assert(r.u16() == 0xBEEF); // still fully buffered
    assert(r.ok());
    uint8_t last = r.u8(); // this is the truncated read
    assert(last == 0);
    assert(!r.ok());

    // Further reads past the failure point continue to return zero and stay
    // failed — no crash, no OOB, no silent recovery.
    assert(r.u32() == 0);
    assert(!r.ok());

    printf("test_truncation: OK\n");
}

void test_overflow() {
    // ByteWriter with a small fixed cap; write more bytes than cap allows.
    uint8_t small[4];
    ByteWriter w(small, sizeof(small));

    w.u16(1); // 2 bytes, fits (pos=2)
    assert(w.ok());
    w.u32(2); // needs 4 more bytes, only 2 remain -> overflow
    assert(!w.ok());
    assert(w.size() == 2); // nothing written by the failed u32 call

    // Further writes stay failed and never touch memory past cap (verified
    // under ASan).
    w.u8(3);
    assert(!w.ok());
    assert(w.size() == 2);

    printf("test_overflow: OK\n");
}

} // namespace

int main() {
    test_round_trip();
    test_truncation();
    test_overflow();
    printf("All bytebuffer tests passed.\n");
    return 0;
}
