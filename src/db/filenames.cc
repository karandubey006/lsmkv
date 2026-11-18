#include "db/filenames.h"

#include <charconv>

namespace lsmkv {
namespace {

constexpr std::string_view kWalSuffix = ".wal";
constexpr std::string_view kSstSuffix = ".sst";
constexpr std::string_view kManifestPrefix = "MANIFEST-";
constexpr size_t kMinDigits = 6;

std::string FormatNumber(uint64_t number) {
  std::string digits = std::to_string(number);
  if (digits.size() < kMinDigits) digits.insert(0, kMinDigits - digits.size(), '0');
  return digits;
}

std::optional<uint64_t> ParseNumber(std::string_view digits) {
  if (digits.size() < kMinDigits) return std::nullopt;
  uint64_t number = 0;
  auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
  if (ec != std::errc() || end != digits.data() + digits.size()) return std::nullopt;
  // Rejects extra leading zeros, so each number has one spelling.
  if (FormatNumber(number) != digits) return std::nullopt;
  return number;
}

}  // namespace

std::string WalFileName(uint64_t number) { return FormatNumber(number) + std::string(kWalSuffix); }

std::string SstFileName(uint64_t number) { return FormatNumber(number) + std::string(kSstSuffix); }

std::string ManifestFileName(uint64_t number) {
  return std::string(kManifestPrefix) + FormatNumber(number);
}

std::optional<ParsedFileName> ParseFileName(std::string_view name) {
  if (name == kLockFileName) return ParsedFileName{FileKind::kLock, 0};
  if (name == kCurrentFileName) return ParsedFileName{FileKind::kCurrent, 0};
  if (name == kCurrentTmpFileName) return ParsedFileName{FileKind::kCurrentTmp, 0};

  FileKind kind;
  std::string_view digits;
  if (name.starts_with(kManifestPrefix)) {
    kind = FileKind::kManifest;
    digits = name.substr(kManifestPrefix.size());
  } else if (name.ends_with(kWalSuffix)) {
    kind = FileKind::kWal;
    digits = name.substr(0, name.size() - kWalSuffix.size());
  } else if (name.ends_with(kSstSuffix)) {
    kind = FileKind::kSst;
    digits = name.substr(0, name.size() - kSstSuffix.size());
  } else {
    return std::nullopt;
  }
  const std::optional<uint64_t> number = ParseNumber(digits);
  if (!number) return std::nullopt;
  return ParsedFileName{kind, *number};
}

std::optional<uint64_t> ParseWalFileName(std::string_view name) {
  const std::optional<ParsedFileName> parsed = ParseFileName(name);
  if (!parsed || parsed->kind != FileKind::kWal) return std::nullopt;
  return parsed->number;
}

}  // namespace lsmkv
