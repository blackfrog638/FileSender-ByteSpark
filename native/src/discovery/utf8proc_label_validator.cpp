#include <utf8proc.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>

#include "xnn_transfer/core/discovery/discovery.hpp"

namespace xnn_transfer::core::discovery {
namespace {

[[nodiscard]] bool IsForbiddenCategory(const utf8proc_category_t category) noexcept {
  return category == UTF8PROC_CATEGORY_CN || category >= UTF8PROC_CATEGORY_CC;
}

class Utf8procDisplayLabelValidator final : public DisplayLabelValidator {
 public:
  [[nodiscard]] bool IsCanonical(
      const std::span<const std::uint8_t> encoded) const noexcept override {
    if (encoded.empty() || encoded.size() > kMaxDisplayLabelBytes) {
      return false;
    }

    std::size_t offset = 0;
    std::size_t scalar_count = 0;
    utf8proc_category_t first_category = UTF8PROC_CATEGORY_CN;
    utf8proc_category_t last_category = UTF8PROC_CATEGORY_CN;
    while (offset < encoded.size()) {
      utf8proc_int32_t codepoint = 0;
      const utf8proc_ssize_t consumed = utf8proc_iterate(
          encoded.data() + offset,
          static_cast<utf8proc_ssize_t>(encoded.size() - offset), &codepoint);
      if (consumed <= 0) {
        return false;
      }

      const utf8proc_category_t category =
          utf8proc_category(static_cast<utf8proc_int32_t>(codepoint));
      if (IsForbiddenCategory(category) || codepoint == 0x2028 || codepoint == 0x2029) {
        return false;
      }
      if (scalar_count == 0) {
        first_category = category;
      }
      last_category = category;
      ++scalar_count;
      if (scalar_count > kMaxDisplayLabelScalars) {
        return false;
      }
      offset += static_cast<std::size_t>(consumed);
    }

    if (first_category == UTF8PROC_CATEGORY_ZS ||
        last_category == UTF8PROC_CATEGORY_ZS) {
      return false;
    }

    utf8proc_uint8_t* normalized = nullptr;
    const auto options = static_cast<utf8proc_option_t>(
        UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_REJECTNA);
    const utf8proc_ssize_t normalized_size =
        utf8proc_map(encoded.data(), static_cast<utf8proc_ssize_t>(encoded.size()),
                     &normalized, options);
    const std::unique_ptr<utf8proc_uint8_t, decltype(&std::free)> owner(normalized,
                                                                        &std::free);
    return normalized_size == static_cast<utf8proc_ssize_t>(encoded.size()) &&
           normalized != nullptr &&
           std::memcmp(encoded.data(), normalized, encoded.size()) == 0;
  }
};

}  // namespace

std::shared_ptr<const DisplayLabelValidator> MakeUtf8procDisplayLabelValidator() {
  return std::make_shared<Utf8procDisplayLabelValidator>();
}

}  // namespace xnn_transfer::core::discovery
