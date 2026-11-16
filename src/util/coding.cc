#include "util/coding.h"

#include <limits>

namespace lsmkv {

void EncodeFixed32(char* dst, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    dst[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
  }
}

void EncodeFixed64(char* dst, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    dst[i] = static_cast<char>((value >> (8 * i)) & 0xFF);
  }
}

uint32_t DecodeFixed32(const char* src) {
  uint32_t result = 0;
  for (int i = 0; i < 4; ++i) {
    result |= static_cast<uint32_t>(static_cast<unsigned char>(src[i])) << (8 * i);
  }
  return result;
}

uint64_t DecodeFixed64(const char* src) {
  uint64_t result = 0;
  for (int i = 0; i < 8; ++i) {
    result |= static_cast<uint64_t>(static_cast<unsigned char>(src[i])) << (8 * i);
  }
  return result;
}

void PutFixed32(std::string* dst, uint32_t value) {
  char buf[4];
  EncodeFixed32(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutFixed64(std::string* dst, uint64_t value) {
  char buf[8];
  EncodeFixed64(buf, value);
  dst->append(buf, sizeof(buf));
}

void PutVarint64(std::string* dst, uint64_t value) {
  while (value >= 0x80) {
    dst->push_back(static_cast<char>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  dst->push_back(static_cast<char>(value));
}

void PutVarint32(std::string* dst, uint32_t value) { PutVarint64(dst, value); }

bool GetVarint64(std::string_view* input, uint64_t* value) {
  uint64_t result = 0;
  // A uint64 needs at most 10 varint bytes (10 * 7 = 70 >= 64).
  for (size_t i = 0; i < input->size() && i < 10; ++i) {
    const uint64_t byte = static_cast<unsigned char>((*input)[i]);
    if (i == 9 && byte > 1) return false;  // would overflow 64 bits
    result |= (byte & 0x7F) << (7 * i);
    if ((byte & 0x80) == 0) {
      *value = result;
      input->remove_prefix(i + 1);
      return true;
    }
  }
  return false;  // truncated, or more than 10 bytes
}

bool GetVarint32(std::string_view* input, uint32_t* value) {
  std::string_view tmp = *input;
  uint64_t v = 0;
  if (!GetVarint64(&tmp, &v) || v > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *value = static_cast<uint32_t>(v);
  *input = tmp;
  return true;
}

int VarintLength(uint64_t value) {
  int len = 1;
  while (value >= 0x80) {
    value >>= 7;
    ++len;
  }
  return len;
}

char* EncodeVarint32(char* dst, uint32_t value) {
  while (value >= 0x80) {
    *dst++ = static_cast<char>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  *dst++ = static_cast<char>(value);
  return dst;
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
  std::string_view input(p, static_cast<size_t>(limit - p));
  if (!GetVarint32(&input, value)) return nullptr;
  return input.data();
}

void PutLengthPrefixed(std::string* dst, std::string_view value) {
  PutVarint32(dst, static_cast<uint32_t>(value.size()));
  dst->append(value);
}

bool GetLengthPrefixed(std::string_view* input, std::string_view* result) {
  std::string_view tmp = *input;
  uint32_t len = 0;
  if (!GetVarint32(&tmp, &len) || tmp.size() < len) return false;
  *result = tmp.substr(0, len);
  tmp.remove_prefix(len);
  *input = tmp;
  return true;
}

}  // namespace lsmkv
