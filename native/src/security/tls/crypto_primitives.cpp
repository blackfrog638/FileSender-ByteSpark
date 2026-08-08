#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "internal.hpp"

namespace xnn_transfer::core::security::tls::internal {
namespace {

using MacPointer = std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)>;
using MacContextPointer = std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)>;

}  // namespace

bool ConstantTimeEqual(const std::span<const std::uint8_t> left,
                       const std::span<const std::uint8_t> right) noexcept {
  return left.size() == right.size() &&
         (left.empty() || CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0);
}

Result<identity::SecretBuffer> HmacSha256Secure(
    const std::span<const std::uint8_t> key,
    const std::span<const std::uint8_t> message) {
  if (key.size() != kSha256Size) {
    return {.error = SecurityError::kInvalidLength};
  }

  MacPointer algorithm(EVP_MAC_fetch(nullptr, "HMAC", nullptr), &EVP_MAC_free);
  if (!algorithm) {
    return {.error = SecurityError::kCryptoFailure};
  }
  MacContextPointer context(EVP_MAC_CTX_new(algorithm.get()), &EVP_MAC_CTX_free);
  if (!context) {
    return {.error = SecurityError::kCryptoFailure};
  }

  char digest_name[] = "SHA256";
  std::array<OSSL_PARAM, 2> parameters = {
      OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0),
      OSSL_PARAM_construct_end(),
  };
  identity::SecretBuffer output(kSha256Size);
  std::size_t output_length = 0;
  const std::span<std::uint8_t> output_bytes = output.mutable_bytes();
  if (EVP_MAC_init(context.get(), key.data(), key.size(), parameters.data()) != 1 ||
      EVP_MAC_update(context.get(), message.data(), message.size()) != 1 ||
      EVP_MAC_final(context.get(), output_bytes.data(), &output_length,
                    output_bytes.size()) != 1 ||
      output_length != output_bytes.size()) {
    return {.error = SecurityError::kCryptoFailure};
  }
  return {
      .value = std::move(output),
      .error = SecurityError::kNone,
  };
}

Result<identity::SecretBuffer> HkdfExpandSha256Secure(
    const std::span<const std::uint8_t> pseudorandom_key,
    const std::span<const std::uint8_t> information, const std::size_t output_length) {
  if (pseudorandom_key.size() != kSha256Size || output_length == 0 ||
      output_length > 255U * kSha256Size) {
    return {.error = SecurityError::kInvalidLength};
  }

  identity::SecretBuffer output(output_length);
  identity::SecretBuffer previous(kSha256Size);
  std::size_t previous_size = 0;
  std::size_t output_offset = 0;
  std::uint16_t counter = 1;
  while (output_offset < output_length) {
    identity::SecretBuffer message(previous_size + information.size() + 1);
    std::span<std::uint8_t> message_bytes = message.mutable_bytes();
    if (previous_size != 0) {
      std::copy_n(previous.bytes().begin(), previous_size, message_bytes.begin());
    }
    std::copy(information.begin(), information.end(),
              message_bytes.begin() + static_cast<std::ptrdiff_t>(previous_size));
    message_bytes.back() = static_cast<std::uint8_t>(counter);

    Result<identity::SecretBuffer> block =
        HmacSha256Secure(pseudorandom_key, message.bytes());
    if (!block.ok()) {
      return {.error = block.error};
    }
    previous = std::move(*block.value);
    previous_size = previous.size();

    const std::size_t remaining = output_length - output_offset;
    const std::size_t copied = std::min(remaining, previous.size());
    std::copy_n(
        previous.bytes().begin(), copied,
        output.mutable_bytes().begin() + static_cast<std::ptrdiff_t>(output_offset));
    output_offset += copied;
    ++counter;
  }
  return {
      .value = std::move(output),
      .error = SecurityError::kNone,
  };
}

}  // namespace xnn_transfer::core::security::tls::internal

namespace xnn_transfer::core::security::tls {

Result<Digest256> Sha256(const std::span<const std::uint8_t> input) {
  Digest256 output{};
  std::size_t output_length = 0;
  if (EVP_Q_digest(nullptr, "SHA256", nullptr, input.data(), input.size(),
                   output.data(), &output_length) != 1 ||
      output_length != output.size()) {
    return {.error = SecurityError::kCryptoFailure};
  }
  return {.value = output, .error = SecurityError::kNone};
}

Result<Digest256> HmacSha256(const std::span<const std::uint8_t> key,
                             const std::span<const std::uint8_t> message) {
  Result<identity::SecretBuffer> secure = internal::HmacSha256Secure(key, message);
  if (!secure.ok()) {
    return {.error = secure.error};
  }
  Digest256 output{};
  std::copy(secure.value->bytes().begin(), secure.value->bytes().end(), output.begin());
  return {.value = output, .error = SecurityError::kNone};
}

Result<bool> VerifySha256(const std::span<const std::uint8_t> value,
                          const std::span<const std::uint8_t> expected_digest) {
  if (expected_digest.size() != kSha256Size) {
    return {.error = SecurityError::kInvalidLength};
  }
  const Result<Digest256> digest = Sha256(value);
  if (!digest.ok()) {
    return {.error = digest.error};
  }
  if (!internal::ConstantTimeEqual(*digest.value, expected_digest)) {
    return {.error = SecurityError::kOutputMismatch};
  }
  return {.value = true, .error = SecurityError::kNone};
}

}  // namespace xnn_transfer::core::security::tls
