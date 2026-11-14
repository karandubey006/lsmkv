#pragma once

// Byte encodings used by every on-disk format. All fixed-width integers are
// little-endian regardless of host byte order, so files are portable.
//
// Varints use 7 bits per byte, low bits first, with the high bit set on every
// byte except the last: 300 = 0b1_0010_1100 encodes as 0xAC 0x02.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lsmkv {

// Fixed-width.
void EncodeFixed32(char* dst, uint32_t value);
void EncodeFixed64(char* dst, uint64_t value);
uint32_t DecodeFixed32(const char* src);
uint64_t DecodeFixed64(const char* src);
void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

// Varint. Get* consume the varint from the front of *input and return true,
// or return false (leaving *input unchanged) if it is truncated or malformed.
void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);
bool GetVarint32(std::string_view* input, uint32_t* value);
bool GetVarint64(std::string_view* input, uint64_t* value);
int VarintLength(uint64_t value);

// Length-prefixed byte string: varint32 length, then the bytes.
void PutLengthPrefixed(std::string* dst, std::string_view value);
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);

}  // namespace lsmkv
