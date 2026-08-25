#include <doctest.h>

#include <cstdint>
#include <limits>

#include "bytebuffer.h"

using ctf::ByteReader;
using ctf::ByteWriter;

TEST_CASE("u8 round-trip including boundary values") {
    uint8_t buf[16];
    const uint8_t values[] = {0, 1, 127, 128, 254, 255};

    ByteWriter w(buf, sizeof(buf));
    for (uint8_t v : values) w.u8(v);
    CHECK(w.ok());
    CHECK(w.size() == 6);

    ByteReader r(buf, w.size());
    for (uint8_t v : values) {
        CHECK(r.u8() == v);
    }
    CHECK(r.ok());
}

TEST_CASE("u16 round-trip in network byte order") {
    uint8_t buf[32];
    const uint16_t values[] = {0, 1, 0x00FF, 0x0100, 0x1234,
                               0x7FFF, 0x8000, 0xFFFF};

    ByteWriter w(buf, sizeof(buf));
    for (uint16_t v : values) w.u16(v);
    CHECK(w.ok());

    // Big-endian on the wire (htons): 5th value 0x1234 at offset 8 -> {0x12, 0x34}.
    CHECK(buf[8] == 0x12);
    CHECK(buf[9] == 0x34);

    ByteReader r(buf, w.size());
    for (uint16_t v : values) {
        CHECK(r.u16() == v);
    }
    CHECK(r.ok());
}

TEST_CASE("u32 round-trip in network byte order") {
    uint8_t buf[64];
    const uint32_t values[] = {0u, 1u, 0x0000FFFFu, 0x00010000u, 0xDEADBEEFu,
                               0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};

    ByteWriter w(buf, sizeof(buf));
    for (uint32_t v : values) w.u32(v);
    CHECK(w.ok());

    ByteReader r(buf, w.size());
    for (uint32_t v : values) {
        CHECK(r.u32() == v);
    }
    CHECK(r.ok());

    // Big-endian: 5th value 0xDEADBEEF at offset 16 -> {DE, AD, BE, EF}.
    CHECK(buf[16] == 0xDE);
    CHECK(buf[17] == 0xAD);
    CHECK(buf[18] == 0xBE);
    CHECK(buf[19] == 0xEF);
}

TEST_CASE("i16 round-trip including negatives") {
    uint8_t buf[32];
    const int16_t values[] = {
        static_cast<int16_t>(std::numeric_limits<int16_t>::min()), -32767, -256,
        -1, 0, 1, 255, 256, 32767};

    ByteWriter w(buf, sizeof(buf));
    for (int16_t v : values) w.i16(v);
    CHECK(w.ok());

    ByteReader r(buf, w.size());
    for (int16_t v : values) {
        CHECK(r.i16() == v);
    }
    CHECK(r.ok());
}

TEST_CASE("writer overflow: writes past cap set the flag, not memory") {
    uint8_t buf[4];
    ByteWriter w(buf, sizeof(buf));

    w.u32(0x11223344u);
    CHECK(w.ok());
    CHECK(w.size() == 4);

    // Any write attempt past cap must be a no-op that flags overflow.
    w.u8(0xFF);
    CHECK_FALSE(w.ok());
    CHECK(w.size() == 4);

    // Partial fit also overflows and must not partially corrupt.
    uint8_t buf2[3];
    ByteWriter w2(buf2, sizeof(buf2));
    w2.u16(0xAABB);
    CHECK(w2.ok());
    w2.u16(0xCCDD);
    CHECK_FALSE(w2.ok());
    CHECK(w2.size() == 2);
}

TEST_CASE("reader underflow: reads past end return zero and flag") {
    uint8_t buf[3] = {0xAA, 0xBB, 0xCC};
    ByteReader r(buf, sizeof(buf));

    CHECK(r.u16() == 0xAABB);
    CHECK(r.ok());

    // Needs 4 bytes, only 1 left: returns 0, sets underflow.
    CHECK(r.u32() == 0);
    CHECK_FALSE(r.ok());

    // Subsequent reads stay flagged; no crash.
    r.u8();
    CHECK_FALSE(r.ok());
}

TEST_CASE("empty buffer reads return zero immediately") {
    uint8_t buf[1] = {0x42};
    ByteReader r(buf, 0);
    CHECK(r.u8() == 0);
    CHECK_FALSE(r.ok());
}

TEST_CASE("zero-cap writer rejects any write") {
    uint8_t buf[1];
    ByteWriter w(buf, 0);
    w.u8(1);
    CHECK_FALSE(w.ok());
}

TEST_CASE("a second multi-byte op after the flag is already set stays a no-op") {
    // Regression test: once overflow_/underflow_ is set, EVERY later call --
    // not just the one that tripped it -- must refuse without touching
    // memory. A guard written as `!flag_ && ...` instead of `flag_ || ...`
    // only catches the *first* offending call; a second u16/u32 call after
    // that would fall through the (now-false) guard and read/write past the
    // buffer, silently, since it stays within the same fixed-size array.
    uint8_t buf[3];
    ByteWriter w(buf, sizeof(buf));
    w.u16(0xAABB);
    CHECK(w.ok());
    w.u16(0xCCDD); // first overflow: pos_(2)+2 > cap_(3)
    CHECK_FALSE(w.ok());
    const size_t size_after_first_overflow = w.size();
    w.u32(0xDEADBEEFu); // second call while already overflowed
    CHECK_FALSE(w.ok());
    CHECK(w.size() == size_after_first_overflow); // must not have advanced

    uint8_t rbuf[3] = {0xAA, 0xBB, 0xCC};
    ByteReader r(rbuf, sizeof(rbuf));
    CHECK(r.u16() == 0xAABB);
    CHECK(r.u16() == 0); // first underflow: pos_(2)+2 > len_(3)
    CHECK_FALSE(r.ok());
    CHECK(r.u32() == 0); // second call while already underflowed
    CHECK_FALSE(r.ok());
}
