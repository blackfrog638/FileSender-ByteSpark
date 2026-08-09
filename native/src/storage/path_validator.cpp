#include <utf8proc.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

#include "xnn_transfer/core/storage/storage.hpp"

namespace xnn_transfer::core::storage {
namespace {

constexpr std::string_view kReservedTemporaryDirectory = ".xnn-transfer-tmp";

[[nodiscard]] bool IsAsciiAlpha(const char value) noexcept {
  return (value >= 'A' && value <= 'Z') ||
         (value >= 'a' && value <= 'z');
}

[[nodiscard]] bool AsciiCaseInsensitiveEquals(const std::string_view left,
                                              const std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const char left_folded = left[index] >= 'A' && left[index] <= 'Z'
                                 ? left[index] - 'A' + 'a'
                                 : left[index];
    const char right_folded = right[index] >= 'A' && right[index] <= 'Z'
                                  ? right[index] - 'A' + 'a'
                                  : right[index];
    if (left_folded != right_folded) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool IsNoncharacter(
    const utf8proc_int32_t codepoint) noexcept {
  return (codepoint >= 0xfdd0 && codepoint <= 0xfdef) ||
         (codepoint & 0xffff) == 0xfffe ||
         (codepoint & 0xffff) == 0xffff;
}

[[nodiscard]] bool IsStrictUtf8(
    const std::span<const std::uint8_t> encoded) noexcept {
  std::size_t offset = 0;
  while (offset < encoded.size()) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t consumed = utf8proc_iterate(
        encoded.data() + offset,
        static_cast<utf8proc_ssize_t>(encoded.size() - offset),
        &codepoint);
    if (consumed <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(consumed);
  }
  return true;
}

[[nodiscard]] ValidationError ValidateCodepoints(
    const std::span<const std::uint8_t> encoded) noexcept {
  std::size_t offset = 0;
  while (offset < encoded.size()) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t consumed = utf8proc_iterate(
        encoded.data() + offset,
        static_cast<utf8proc_ssize_t>(encoded.size() - offset),
        &codepoint);
    if (consumed <= 0) {
      return ValidationError::kInvalidUtf8;
    }
    if (codepoint == 0) {
      return ValidationError::kPathNul;
    }
    if ((codepoint >= 1 && codepoint <= 0x1f) ||
        codepoint == 0x7f) {
      return ValidationError::kPathC0Control;
    }
    if (codepoint >= 0x80 && codepoint <= 0x9f) {
      return ValidationError::kPathC1Control;
    }
    if (IsNoncharacter(codepoint)) {
      return ValidationError::kPathNoncharacter;
    }
    offset += static_cast<std::size_t>(consumed);
  }
  return ValidationError::kNone;
}

[[nodiscard]] bool IsNfc(
    const std::span<const std::uint8_t> encoded) noexcept {
  utf8proc_uint8_t* normalized = nullptr;
  const auto options =
      static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE);
  const utf8proc_ssize_t normalized_size = utf8proc_map(
      encoded.data(), static_cast<utf8proc_ssize_t>(encoded.size()),
      &normalized, options);
  const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owner(
      normalized, &std::free);
  return normalized_size ==
             static_cast<utf8proc_ssize_t>(encoded.size()) &&
         normalized != nullptr &&
         std::memcmp(encoded.data(), normalized, encoded.size()) == 0;
}

}  // namespace

PathValidationResult ValidateReceivePath(
    const std::span<const std::uint8_t> encoded) {
  if (encoded.empty()) {
    return PathValidationResult(ValidationError::kPathEmpty);
  }
  if (encoded.size() > kMaxRelativePathBytes) {
    return PathValidationResult(ValidationError::kPathBytesLimit);
  }
  if (!IsStrictUtf8(encoded)) {
    return PathValidationResult(ValidationError::kInvalidUtf8);
  }

  const std::string path(
      reinterpret_cast<const char*>(encoded.data()), encoded.size());
  if (path.starts_with("//") || path.starts_with("\\\\")) {
    return PathValidationResult(ValidationError::kPathUnc);
  }
  if (path.starts_with('/')) {
    return PathValidationResult(ValidationError::kPathAbsolute);
  }
  if (path.size() >= 2 && IsAsciiAlpha(path[0]) && path[1] == ':') {
    return PathValidationResult(
        path.size() >= 3 &&
                (path[2] == '/' || path[2] == '\\')
            ? ValidationError::kPathDriveAbsolute
            : ValidationError::kPathDriveQualified);
  }
  if (path.find('\\') != std::string::npos) {
    return PathValidationResult(ValidationError::kPathBackslash);
  }
  if (path.find(':') != std::string::npos) {
    return PathValidationResult(ValidationError::kPathColonOrAds);
  }
  if (path.ends_with('/')) {
    return PathValidationResult(
        ValidationError::kPathTrailingSeparator);
  }

  const ValidationError codepoint_error = ValidateCodepoints(encoded);
  if (codepoint_error != ValidationError::kNone) {
    return PathValidationResult(codepoint_error);
  }
  if (!IsNfc(encoded)) {
    return PathValidationResult(ValidationError::kPathNotNfc);
  }

  std::vector<std::string> components;
  std::size_t component_start = 0;
  while (component_start < path.size()) {
    const std::size_t separator = path.find('/', component_start);
    const std::size_t component_end =
        separator == std::string::npos ? path.size() : separator;
    const std::string_view component(
        path.data() + component_start,
        component_end - component_start);
    if (component.empty()) {
      return PathValidationResult(
          ValidationError::kPathEmptyComponent);
    }
    if (component == ".") {
      return PathValidationResult(
          ValidationError::kPathDotComponent);
    }
    if (component == "..") {
      return PathValidationResult(ValidationError::kPathTraversal);
    }
    if (components.empty() &&
        AsciiCaseInsensitiveEquals(component, kReservedTemporaryDirectory)) {
      return PathValidationResult(ValidationError::kPathReservedComponent);
    }
    components.emplace_back(component);
    if (components.size() > kMaxPathComponents) {
      return PathValidationResult(
          ValidationError::kPathComponentCountLimit);
    }
    if (component.size() > kMaxPathComponentBytes) {
      return PathValidationResult(
          ValidationError::kPathComponentBytesLimit);
    }
    if (separator == std::string::npos) {
      break;
    }
    component_start = separator + 1;
  }

  return PathValidationResult(path, std::move(components));
}

RequestValidationResult ValidateReceiveRequest(
    const std::span<const std::uint8_t> encoded_path,
    const std::uint64_t declared_size,
    const std::uint64_t local_max_file_bytes) {
  const PathValidationResult path = ValidateReceivePath(encoded_path);
  if (!path.ok()) {
    return RequestValidationResult(path.error());
  }
  const std::uint64_t effective_max =
      std::min(local_max_file_bytes, kMaxFileBytes);
  if (declared_size > effective_max) {
    return RequestValidationResult(
        ValidationError::kDeclaredSizeLimit);
  }

  return RequestValidationResult(*path.path(), declared_size);
}

}  // namespace xnn_transfer::core::storage
