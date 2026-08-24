#include "bytebuffer.h"

#include <arpa/inet.h>
#include <cstring>

// README §5.2: cursor-based, endian-safe read/write. Multi-byte values go on
// the wire in network byte order via htons/htonl (and their ntoh inverses on
// read) so the byte layout never depends on the host's endianness. Never
// memcpy a struct onto the wire (README §5.3) — every value is written and
// read one field at a time through these primitives instead.
//
// A malformed or short buffer must never crash or hang the caller (README
// §5.2): ByteWriter refuses to write past `cap_` and sets `overflow_` instead
// of writing out of bounds; ByteReader refuses to read past `len_`, returns
// zero, and sets `underflow_` instead of reading out of bounds. Once either
// flag is set it stays set — a writer/reader that has already failed must not
// silently recover and produce a partially-valid buffer.

namespace ctf {

ByteWriter::ByteWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

void ByteWriter::u8(uint8_t v) {
    if (overflow_ || pos_ + 1 > cap_) {
        overflow_ = true;
        return;
    }
    buf_[pos_] = v;
    pos_ += 1;
}

void ByteWriter::u16(uint16_t v) {
    if (overflow_ || pos_ + 2 > cap_) {
        overflow_ = true;
        return;
    }
    uint16_t net = htons(v);
    std::memcpy(buf_ + pos_, &net, 2);
    pos_ += 2;
}

void ByteWriter::u32(uint32_t v) {
    if (overflow_ || pos_ + 4 > cap_) {
        overflow_ = true;
        return;
    }
    uint32_t net = htonl(v);
    std::memcpy(buf_ + pos_, &net, 4);
    pos_ += 4;
}

void ByteWriter::i16(int16_t v) {
    // Same wire representation as u16 — reinterpret the bit pattern, do not
    // reinterpret the value (a signed-to-unsigned cast preserves bits, never
    // triggers UB here, and round-trips exactly through the matching
    // ByteReader::i16 on the other side).
    u16(static_cast<uint16_t>(v));
}

ByteReader::ByteReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

uint8_t ByteReader::u8() {
    if (underflow_ || pos_ + 1 > len_) {
        underflow_ = true;
        return 0;
    }
    uint8_t v = buf_[pos_];
    pos_ += 1;
    return v;
}

uint16_t ByteReader::u16() {
    if (underflow_ || pos_ + 2 > len_) {
        underflow_ = true;
        return 0;
    }
    uint16_t net;
    std::memcpy(&net, buf_ + pos_, 2);
    pos_ += 2;
    return ntohs(net);
}

uint32_t ByteReader::u32() {
    if (underflow_ || pos_ + 4 > len_) {
        underflow_ = true;
        return 0;
    }
    uint32_t net;
    std::memcpy(&net, buf_ + pos_, 4);
    pos_ += 4;
    return ntohl(net);
}

int16_t ByteReader::i16() {
    return static_cast<int16_t>(u16());
}

} // namespace ctf
