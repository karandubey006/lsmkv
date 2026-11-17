#include "db/filenames.h"

#include <charconv>

namespace lsmkv {
namespace {

constexpr std::string_view kWalSuffix = ".wal";
constexpr size_t kMinDigits = 6;

}  // namespace

std::string WalFileName(uint64_t number) {
  std::string digits = std::to_string(number);
  if (digits.size() < kMinDigits) digits.insert(0, kMinDigits - digits.size(), '0');
  return digits + std::string(kWalSuffix);
}

std::optional<uint64_t> ParseWalFileName(std::string_view name) {
  if (!name.ends_with(kWalSuffix)) return std::nullopt;
  const std::string_view digits = name.substr(0, name.size() - kWalSuffix.size());
  if (digits.size() < kMinDigits) return std::nullopt;
  uint64_t number = 0;
  auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
  if (ec != std::errc() || end != digits.data() + digits.size()) return std::nullopt;
  // Only the canonical spelling, so each number has exactly one file name.
  if (WalFileName(number) != name) return std::nullopt;
  return number;
}

}  // namespace lsmkv
