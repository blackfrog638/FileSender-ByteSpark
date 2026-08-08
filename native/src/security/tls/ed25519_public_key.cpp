#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "xnn_transfer/core/security/tls/security_profile.hpp"

namespace xnn_transfer::core::security::tls {
namespace {

constexpr std::size_t kFieldLimbCount = 16;
constexpr std::uint64_t kLimbMask = 0xffffU;
constexpr std::uint16_t kTopLimbMask = 0x7fffU;

using FieldLimbs = std::array<std::uint16_t, kFieldLimbCount>;
using WideFieldLimbs = std::array<std::uint64_t, kFieldLimbCount * 2>;

constexpr FieldLimbs kFieldPrime = {
    0xffedU, 0xffffU, 0xffffU, 0xffffU, 0xffffU, 0xffffU, 0xffffU, 0xffffU,
    0xffffU, 0xffffU, 0xffffU, 0xffffU, 0xffffU, 0xffffU, 0xffffU, 0x7fffU,
};

constexpr std::array<std::uint8_t, 32> kInverseExponent = [] {
  std::array<std::uint8_t, 32> value{};
  value.fill(0xffU);
  value[0] = 0xebU;
  value[31] = 0x7fU;
  return value;
}();

constexpr std::array<std::uint8_t, 32> kSquareRootExponent = [] {
  std::array<std::uint8_t, 32> value{};
  value.fill(0xffU);
  value[0] = 0xfeU;
  value[31] = 0x0fU;
  return value;
}();

constexpr std::array<std::uint8_t, 32> kSquareRootMinusOneExponent = [] {
  std::array<std::uint8_t, 32> value{};
  value.fill(0xffU);
  value[0] = 0xfbU;
  value[31] = 0x1fU;
  return value;
}();

constexpr std::array<std::uint8_t, 32> kPrimeSubgroupOrder = {
    0xedU, 0xd3U, 0xf5U, 0x5cU, 0x1aU, 0x63U, 0x12U, 0x58U, 0xd6U, 0x9cU, 0xf7U,
    0xa2U, 0xdeU, 0xf9U, 0xdeU, 0x14U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x10U,
};

[[nodiscard]] bool LimbsLessThan(const FieldLimbs& left,
                                 const FieldLimbs& right) noexcept {
  for (std::size_t index = kFieldLimbCount; index > 0; --index) {
    const std::size_t limb = index - 1;
    if (left[limb] != right[limb]) {
      return left[limb] < right[limb];
    }
  }
  return false;
}

[[nodiscard]] FieldLimbs SubtractLimbs(const FieldLimbs& left,
                                       const FieldLimbs& right) noexcept {
  FieldLimbs result{};
  std::uint32_t borrow = 0;
  for (std::size_t index = 0; index < kFieldLimbCount; ++index) {
    const std::uint32_t minuend = left[index];
    const std::uint32_t subtrahend = static_cast<std::uint32_t>(right[index]) + borrow;
    if (minuend < subtrahend) {
      result[index] = static_cast<std::uint16_t>(minuend + 0x1'0000U - subtrahend);
      borrow = 1;
    } else {
      result[index] = static_cast<std::uint16_t>(minuend - subtrahend);
      borrow = 0;
    }
  }
  return result;
}

[[nodiscard]] FieldLimbs Reduce(WideFieldLimbs value) noexcept {
  // Schoolbook coefficients stay below 2^37 with 16-bit limbs. Folding via
  // 2^256 == 38 (mod 2^255-19) therefore remains within uint64_t.
  for (std::size_t index = value.size(); index > kFieldLimbCount; --index) {
    const std::size_t high = index - 1;
    value[high - kFieldLimbCount] += 38U * value[high];
    value[high] = 0;
  }

  for (;;) {
    std::uint64_t high_word = 0;
    for (std::size_t index = 0; index < kFieldLimbCount; ++index) {
      const std::uint64_t carry = value[index] >> 16U;
      value[index] &= kLimbMask;
      if (index + 1 < kFieldLimbCount) {
        value[index + 1] += carry;
      } else {
        high_word = carry;
      }
    }
    if (high_word != 0) {
      value[0] += 38U * high_word;
      continue;
    }

    const std::uint64_t high_bit = value[15] >> 15U;
    value[15] &= kTopLimbMask;
    if (high_bit != 0) {
      value[0] += 19U * high_bit;
      continue;
    }
    break;
  }

  FieldLimbs result{};
  for (std::size_t index = 0; index < kFieldLimbCount; ++index) {
    result[index] = static_cast<std::uint16_t>(value[index]);
  }
  if (!LimbsLessThan(result, kFieldPrime)) {
    result = SubtractLimbs(result, kFieldPrime);
  }
  return result;
}

class FieldElement final {
 public:
  FieldElement() = default;

  [[nodiscard]] static FieldElement FromUint(const std::uint32_t value) noexcept {
    FieldLimbs limbs{};
    limbs[0] = static_cast<std::uint16_t>(value & 0xffffU);
    limbs[1] = static_cast<std::uint16_t>(value >> 16U);
    return FieldElement(limbs);
  }

  [[nodiscard]] static FieldElement FromCanonicalLittleEndian(
      const std::span<const std::uint8_t, 32> encoded) noexcept {
    FieldLimbs limbs{};
    for (std::size_t index = 0; index < kFieldLimbCount; ++index) {
      const std::uint16_t low = encoded[index * 2];
      const std::uint16_t high = encoded[index * 2 + 1];
      limbs[index] = static_cast<std::uint16_t>(low | (high << 8U));
    }
    return FieldElement(limbs);
  }

  [[nodiscard]] static bool IsCanonicalLittleEndian(
      const std::span<const std::uint8_t, 32> encoded) noexcept {
    FieldLimbs limbs{};
    for (std::size_t index = 0; index < kFieldLimbCount; ++index) {
      const std::uint16_t low = encoded[index * 2];
      const std::uint16_t high = encoded[index * 2 + 1];
      limbs[index] = static_cast<std::uint16_t>(low | (high << 8U));
    }
    return LimbsLessThan(limbs, kFieldPrime);
  }

  [[nodiscard]] bool IsZero() const noexcept {
    for (const std::uint16_t limb : limbs_) {
      if (limb != 0) {
        return false;
      }
    }
    return true;
  }

  [[nodiscard]] bool IsOdd() const noexcept { return (limbs_[0] & 1U) != 0; }

  friend bool operator==(const FieldElement&, const FieldElement&) = default;

  [[nodiscard]] friend FieldElement operator+(const FieldElement& left,
                                              const FieldElement& right) noexcept {
    WideFieldLimbs sum{};
    for (std::size_t index = 0; index < kFieldLimbCount; ++index) {
      sum[index] = static_cast<std::uint64_t>(left.limbs_[index]) +
                   static_cast<std::uint64_t>(right.limbs_[index]);
    }
    return FieldElement(Reduce(sum));
  }

  [[nodiscard]] friend FieldElement operator-(const FieldElement& left,
                                              const FieldElement& right) noexcept {
    if (!LimbsLessThan(left.limbs_, right.limbs_)) {
      return FieldElement(SubtractLimbs(left.limbs_, right.limbs_));
    }
    const FieldLimbs difference = SubtractLimbs(right.limbs_, left.limbs_);
    return FieldElement(SubtractLimbs(kFieldPrime, difference));
  }

  [[nodiscard]] friend FieldElement operator*(const FieldElement& left,
                                              const FieldElement& right) noexcept {
    WideFieldLimbs product{};
    for (std::size_t left_index = 0; left_index < kFieldLimbCount; ++left_index) {
      for (std::size_t right_index = 0; right_index < kFieldLimbCount; ++right_index) {
        product[left_index + right_index] +=
            static_cast<std::uint64_t>(left.limbs_[left_index]) *
            static_cast<std::uint64_t>(right.limbs_[right_index]);
      }
    }
    return FieldElement(Reduce(product));
  }

  [[nodiscard]] FieldElement Square() const noexcept { return *this * *this; }

  [[nodiscard]] FieldElement Pow(
      const std::array<std::uint8_t, 32>& exponent) const noexcept {
    FieldElement result = FromUint(1);
    for (int bit = 254; bit >= 0; --bit) {
      result = result.Square();
      const auto byte_index = static_cast<std::size_t>(bit / 8);
      const auto bit_index = static_cast<unsigned>(bit % 8);
      if (((exponent[byte_index] >> bit_index) & 1U) != 0) {
        result = result * *this;
      }
    }
    return result;
  }

 private:
  explicit FieldElement(FieldLimbs limbs) noexcept : limbs_(std::move(limbs)) {}

  FieldLimbs limbs_{};
};

[[nodiscard]] const FieldElement& CurveD() noexcept {
  static const FieldElement value =
      FieldElement{} - FieldElement::FromUint(121'665) *
                           FieldElement::FromUint(121'666).Pow(kInverseExponent);
  return value;
}

[[nodiscard]] const FieldElement& SquareRootMinusOne() noexcept {
  static const FieldElement value =
      FieldElement::FromUint(2).Pow(kSquareRootMinusOneExponent);
  return value;
}

struct EdwardsPoint {
  FieldElement x{};
  FieldElement y{FieldElement::FromUint(1)};
  FieldElement z{FieldElement::FromUint(1)};
  FieldElement t{};
};

[[nodiscard]] EdwardsPoint Add(const EdwardsPoint& left,
                               const EdwardsPoint& right) noexcept {
  const FieldElement a = (left.y - left.x) * (right.y - right.x);
  const FieldElement b = (left.y + left.x) * (right.y + right.x);
  const FieldElement c = FieldElement::FromUint(2) * CurveD() * left.t * right.t;
  const FieldElement d = FieldElement::FromUint(2) * left.z * right.z;
  const FieldElement e = b - a;
  const FieldElement f = d - c;
  const FieldElement g = d + c;
  const FieldElement h = b + a;
  return EdwardsPoint{
      .x = e * f,
      .y = g * h,
      .z = f * g,
      .t = e * h,
  };
}

[[nodiscard]] bool IsIdentity(const EdwardsPoint& point) noexcept {
  return point.x.IsZero() && point.y == point.z;
}

[[nodiscard]] EdwardsPoint MultiplyByPrimeSubgroupOrder(
    const EdwardsPoint& point, std::size_t* const loop_count) noexcept {
  EdwardsPoint result{};
  EdwardsPoint addend = point;
  for (std::size_t bit = 0; bit < kPrimeSubgroupOrder.size() * 8; ++bit) {
    if (loop_count != nullptr) {
      ++*loop_count;
    }
    const std::uint8_t scalar_byte = kPrimeSubgroupOrder[bit / 8];
    const auto scalar_bit = static_cast<unsigned>(bit % 8);
    if (((scalar_byte >> scalar_bit) & 1U) != 0) {
      result = Add(result, addend);
    }
    addend = Add(addend, addend);
  }
  return result;
}

struct DecodePointResult {
  EdwardsPoint point{};
  SecurityError error{SecurityError::kNone};
};

[[nodiscard]] DecodePointResult DecodePoint(
    const std::span<const std::uint8_t, 32> encoded) noexcept {
  std::array<std::uint8_t, 32> y_bytes{};
  std::copy(encoded.begin(), encoded.end(), y_bytes.begin());
  const bool x_sign = (y_bytes[31] & 0x80U) != 0;
  y_bytes[31] &= 0x7fU;

  const std::span<const std::uint8_t, 32> canonical_y(y_bytes);
  if (!FieldElement::IsCanonicalLittleEndian(canonical_y)) {
    return {.error = SecurityError::kNonCanonicalEncoding};
  }

  const FieldElement y = FieldElement::FromCanonicalLittleEndian(canonical_y);
  const FieldElement y_squared = y.Square();
  const FieldElement numerator = y_squared - FieldElement::FromUint(1);
  const FieldElement denominator = CurveD() * y_squared + FieldElement::FromUint(1);
  if (denominator.IsZero()) {
    return {.error = SecurityError::kInvalidPublicKey};
  }

  const FieldElement x_squared = numerator * denominator.Pow(kInverseExponent);
  FieldElement x = x_squared.Pow(kSquareRootExponent);
  if (x.Square() != x_squared) {
    x = x * SquareRootMinusOne();
  }
  if (x.Square() != x_squared) {
    return {.error = SecurityError::kInvalidPublicKey};
  }
  if (x.IsZero() && x_sign) {
    return {.error = SecurityError::kNonCanonicalEncoding};
  }
  if (x.IsOdd() != x_sign) {
    x = FieldElement{} - x;
  }

  return {
      .point =
          EdwardsPoint{
              .x = x,
              .y = y,
              .z = FieldElement::FromUint(1),
              .t = x * y,
          },
      .error = SecurityError::kNone,
  };
}

}  // namespace

namespace internal {

std::size_t PrimeSubgroupOrderLoopCountForTesting() noexcept {
  std::size_t loop_count = 0;
  static_cast<void>(MultiplyByPrimeSubgroupOrder(EdwardsPoint{}, &loop_count));
  return loop_count;
}

}  // namespace internal

Result<ValidatedEd25519PublicKey> ValidateEd25519PublicKey(
    const std::span<const std::uint8_t> encoded) {
  if (encoded.size() != kEd25519PublicKeySize) {
    return {.error = SecurityError::kInvalidLength};
  }

  const std::span<const std::uint8_t, kEd25519PublicKeySize> fixed(encoded.data(),
                                                                   encoded.size());
  const DecodePointResult decoded = DecodePoint(fixed);
  if (decoded.error != SecurityError::kNone) {
    return {.error = decoded.error};
  }
  if (IsIdentity(decoded.point) ||
      !IsIdentity(MultiplyByPrimeSubgroupOrder(decoded.point, nullptr))) {
    return {.error = SecurityError::kInvalidPublicKey};
  }

  identity::PublicKey bytes{};
  std::copy(encoded.begin(), encoded.end(), bytes.begin());
  return {
      .value = ValidatedEd25519PublicKey(bytes),
      .error = SecurityError::kNone,
  };
}

identity::Result<void> OpenSslPeerPublicKeyValidator::Validate(
    const identity::PublicKey& public_key) {
  const auto result = ValidateEd25519PublicKey(public_key);
  return result.ok()
             ? identity::Result<void>::Success()
             : identity::Result<void>::Failure(identity::ErrorCode::kInvalidArgument);
}

}  // namespace xnn_transfer::core::security::tls
