#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lsmkv {

// The result of every fallible operation; lsmkv never throws. Marked
// [[nodiscard]] so silently dropping an error is a compiler warning.
//
// Cost model: an OK status is a single null pointer, so the success path never
// allocates. Only errors carry a heap-allocated code + message.
class [[nodiscard]] Status {
 public:
  enum class Code : uint8_t {
    kOk = 0,
    kNotFound,
    kCorruption,
    kNotSupported,
    kInvalidArgument,
    kIOError,
  };

  Status() noexcept = default;  // OK
  Status(const Status& other);
  Status& operator=(const Status& other);
  Status(Status&&) noexcept = default;
  Status& operator=(Status&&) noexcept = default;
  ~Status() = default;

  static Status OK() { return Status(); }
  static Status NotFound(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kNotFound, msg, msg2);
  }
  static Status Corruption(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kCorruption, msg, msg2);
  }
  static Status NotSupported(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kNotSupported, msg, msg2);
  }
  static Status InvalidArgument(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kInvalidArgument, msg, msg2);
  }
  static Status IOError(std::string_view msg, std::string_view msg2 = {}) {
    return Status(Code::kIOError, msg, msg2);
  }

  bool ok() const { return state_ == nullptr; }
  bool IsNotFound() const { return code() == Code::kNotFound; }
  bool IsCorruption() const { return code() == Code::kCorruption; }
  bool IsNotSupported() const { return code() == Code::kNotSupported; }
  bool IsInvalidArgument() const { return code() == Code::kInvalidArgument; }
  bool IsIOError() const { return code() == Code::kIOError; }

  Code code() const { return state_ ? state_->code : Code::kOk; }

  // "OK", or "<Code>: msg[: msg2]".
  std::string ToString() const;

 private:
  struct State {
    Code code;
    std::string message;
  };

  Status(Code code, std::string_view msg, std::string_view msg2);

  std::unique_ptr<State> state_;  // nullptr means OK
};

}  // namespace lsmkv
