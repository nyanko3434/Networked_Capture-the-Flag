#include "bytebuffer.h"

// Scaffolding only — real read/write bodies are written together in the
// shared/ pairing session (README §11 week 1).

namespace ctf {

ByteWriter::ByteWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

void ByteWriter::u8(uint8_t) {}
void ByteWriter::u16(uint16_t) {}
void ByteWriter::u32(uint32_t) {}
void ByteWriter::i16(int16_t) {}

ByteReader::ByteReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

uint8_t ByteReader::u8() { return 0; }
uint16_t ByteReader::u16() { return 0; }
uint32_t ByteReader::u32() { return 0; }
int16_t ByteReader::i16() { return 0; }

} // namespace ctf
