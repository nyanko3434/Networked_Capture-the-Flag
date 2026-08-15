#pragma once

// Cursor-based, endian-safe read/write over a raw byte buffer (README §5.2).
//
// Never memcpy a struct onto the wire: padding and endianness differ between
// machines. Every message gets an explicit encode/decode pair built on top
// of these two classes.
//
// A malformed packet must never crash or hang the server: ByteReader returns
// zeros past the end of the buffer and sets `underflow`. Every decode path
// must check ok() and drop the packet on failure.

#include <cstddef>
#include <cstdint>

namespace ctf {

class ByteWriter {
public:
    ByteWriter(uint8_t* buf, size_t cap);

    void u8(uint8_t v);
    void u16(uint16_t v); // network byte order (htons)
    void u32(uint32_t v); // network byte order (htonl)
    void i16(int16_t v);

    bool ok() const { return !overflow_; }
    size_t size() const { return pos_; }

private:
    uint8_t* buf_;
    size_t cap_;
    size_t pos_ = 0;
    bool overflow_ = false;
};

class ByteReader {
public:
    ByteReader(const uint8_t* buf, size_t len);

    uint8_t u8();
    uint16_t u16(); // network byte order (ntohs)
    uint32_t u32(); // network byte order (ntohl)
    int16_t i16();

    bool ok() const { return !underflow_; }

private:
    const uint8_t* buf_;
    size_t len_;
    size_t pos_ = 0;
    bool underflow_ = false;
};

} // namespace ctf
