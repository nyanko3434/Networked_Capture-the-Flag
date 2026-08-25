#include "bytebuffer.h"

// Cursor-based, endian-safe read/write (README §5.2). Network byte order
// throughout; reads past the end return zero and set `underflow`, writes
// past cap are no-ops that set `overflow` — a malformed packet must never
// crash or hang the server.
//
// Multi-byte operations are atomic: the full width is checked up front so a
// failing op consumes nothing and writes nothing (no torn half-values).

namespace ctf {

ByteWriter::ByteWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

void ByteWriter::u8(uint8_t v) {
    if (overflow_ || pos_ + 1 > cap_) {
        overflow_ = true;
        return;
    }
    buf_[pos_++] = v;
}

void ByteWriter::u16(uint16_t v) {
    // `overflow_ ||` (not `!overflow_ &&`) so a writer that already failed
    // stays refused on every later call too -- otherwise, once overflow_
    // was set by some earlier op, this guard would be skipped entirely and
    // the write below would run anyway, past `cap_`.
    if (overflow_ || pos_ + 2 > cap_) {
        overflow_ = true;
        return;
    }
    buf_[pos_++] = static_cast<uint8_t>(v >> 8);
    buf_[pos_++] = static_cast<uint8_t>(v & 0xFF);
}

void ByteWriter::u32(uint32_t v) {
    if (overflow_ || pos_ + 4 > cap_) {
        overflow_ = true;
        return;
    }
    buf_[pos_++] = static_cast<uint8_t>(v >> 24);
    buf_[pos_++] = static_cast<uint8_t>((v >> 16) & 0xFF);
    buf_[pos_++] = static_cast<uint8_t>((v >> 8) & 0xFF);
    buf_[pos_++] = static_cast<uint8_t>(v & 0xFF);
}

void ByteWriter::i16(int16_t v) { u16(static_cast<uint16_t>(v)); }

ByteReader::ByteReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

uint8_t ByteReader::u8() {
    if (underflow_ || pos_ + 1 > len_) {
        underflow_ = true;
        return 0;
    }
    return buf_[pos_++];
}

uint16_t ByteReader::u16() {
    // Same fix as ByteWriter::u16/u32 above: `underflow_ ||`, not
    // `!underflow_ &&`, so a reader that already failed keeps refusing
    // instead of reading past `len_` on the next call.
    if (underflow_ || pos_ + 2 > len_) {
        underflow_ = true;
        return 0;
    }
    const uint16_t hi = buf_[pos_++];
    const uint16_t lo = buf_[pos_++];
    return static_cast<uint16_t>((hi << 8) | lo);
}

uint32_t ByteReader::u32() {
    if (underflow_ || pos_ + 4 > len_) {
        underflow_ = true;
        return 0;
    }
    const uint32_t b0 = buf_[pos_++];
    const uint32_t b1 = buf_[pos_++];
    const uint32_t b2 = buf_[pos_++];
    const uint32_t b3 = buf_[pos_++];
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
}

int16_t ByteReader::i16() { return static_cast<int16_t>(u16()); }

} // namespace ctf
