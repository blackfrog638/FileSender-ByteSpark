#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "xnn_transfer/core/security/tls/security_profile.hpp"

namespace xnn_transfer::core::security::tls::internal {

[[nodiscard]] std::size_t PrimeSubgroupOrderLoopCountForTesting() noexcept;

}  // namespace xnn_transfer::core::security::tls::internal

namespace {

using xnn_transfer::core::security::identity::ErrorCode;
using xnn_transfer::core::security::identity::PublicKey;
using xnn_transfer::core::security::tls::OpenSslPeerPublicKeyValidator;
using xnn_transfer::core::security::tls::SecurityError;
using xnn_transfer::core::security::tls::ValidateEd25519PublicKey;

using Bytes = std::vector<std::uint8_t>;
using PkeyPointer = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

int failures = 0;

void Expect(const bool condition, const std::string_view message) {
  if (condition) {
    return;
  }
  std::cerr << "FAILED: " << message << '\n';
  ++failures;
}

Bytes DecodeHex(const std::string_view encoded) {
  auto nibble = [](const char value) -> int {
    if (value >= '0' && value <= '9') {
      return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
      return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
      return value - 'A' + 10;
    }
    return -1;
  };

  Bytes output;
  if ((encoded.size() & 1U) != 0U) {
    return output;
  }
  output.reserve(encoded.size() / 2);
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    const int high = nibble(encoded[offset]);
    const int low = nibble(encoded[offset + 1]);
    if (high < 0 || low < 0) {
      return {};
    }
    output.push_back(static_cast<std::uint8_t>((high << 4) | low));
  }
  return output;
}

void ExpectAccepted(const std::string_view encoded,
                    const std::string_view description) {
  const Bytes bytes = DecodeHex(encoded);
  const auto result = ValidateEd25519PublicKey(bytes);
  Expect(result.ok(), description);
  if (result.ok()) {
    Expect(std::equal(result.value->bytes().begin(), result.value->bytes().end(),
                      bytes.begin(), bytes.end()),
           "validated key preserves exact canonical pin bytes");
  }
}

void ExpectRejected(const std::string_view encoded, const SecurityError error,
                    const std::string_view description) {
  const Bytes bytes = DecodeHex(encoded);
  const auto result = ValidateEd25519PublicKey(bytes);
  Expect(!result.ok() && result.error == error, description);
}

void TestPublishedPrimeOrderKeys() {
  ExpectAccepted("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                 "RFC 8032 TEST 1 public key is accepted");
  ExpectAccepted("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                 "RFC 8032 TEST 2 public key is accepted");
  ExpectAccepted("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
                 "RFC 8032 TEST 3 public key is accepted");
}

void TestOpenSslDerivedPublicKeys() {
  constexpr std::size_t kSeedCount = 512;
  for (std::size_t seed_index = 0; seed_index < kSeedCount; ++seed_index) {
    std::array<std::uint8_t, 32> seed{};
    seed[0] = static_cast<std::uint8_t>(seed_index);
    seed[1] = static_cast<std::uint8_t>(seed_index >> 8U);
    for (std::size_t byte_index = 2; byte_index < seed.size(); ++byte_index) {
      seed[byte_index] =
          static_cast<std::uint8_t>(0x5dU + seed_index * 0x9dU + byte_index * 0x3bU);
    }

    PkeyPointer key(EVP_PKEY_new_raw_private_key_ex(nullptr, "ED25519", nullptr,
                                                    seed.data(), seed.size()),
                    &EVP_PKEY_free);
    Expect(key != nullptr, "OpenSSL accepts fixed Ed25519 test seed");
    if (!key) {
      continue;
    }

    PublicKey public_key{};
    std::size_t public_key_size = public_key.size();
    const int derived =
        EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &public_key_size);
    Expect(derived == 1 && public_key_size == public_key.size(),
           "OpenSSL derives 32-byte Ed25519 public key");
    if (derived != 1 || public_key_size != public_key.size()) {
      continue;
    }

    const auto result = ValidateEd25519PublicKey(public_key);
    Expect(result.ok(), "native validation accepts OpenSSL-derived key");
  }
}

void TestHostileSubgroupKeys() {
  ExpectRejected("0100000000000000000000000000000000000000000000000000000000000000",
                 SecurityError::kInvalidPublicKey, "Edwards25519 identity is rejected");
  ExpectRejected("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                 SecurityError::kInvalidPublicKey,
                 "canonical order-2 point is rejected");
  ExpectRejected("16a567fe7d4ef5482ab4012c369bf8c5f11e8d0c2559dcda50fde59708f8aee5",
                 SecurityError::kInvalidPublicKey,
                 "mixed-order point outside prime subgroup is rejected");
}

void TestYCoordinateAndSignBoundaries() {
  ExpectRejected("0000000000000000000000000000000000000000000000000000000000000000",
                 SecurityError::kInvalidPublicKey,
                 "canonical y zero with clear sign is rejected");
  ExpectRejected("0000000000000000000000000000000000000000000000000000000000000080",
                 SecurityError::kInvalidPublicKey,
                 "canonical y zero with set sign is rejected");
  ExpectRejected("ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                 SecurityError::kNonCanonicalEncoding,
                 "x zero at p minus one rejects set sign");
  ExpectRejected("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
                 SecurityError::kNonCanonicalEncoding, "y equal to p is noncanonical");
  ExpectAccepted("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707519a",
                 "opposite sign of prime-order point is accepted");
  ExpectRejected("606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7eff",
                 SecurityError::kInvalidPublicKey, "non-curve y encoding is rejected");
}

void TestNonCanonicalAndInvalidEncodings() {
  ExpectRejected("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                 SecurityError::kNonCanonicalEncoding, "y equal to p is noncanonical");
  ExpectRejected("0100000000000000000000000000000000000000000000000000000000000080",
                 SecurityError::kNonCanonicalEncoding,
                 "x zero with sign bit is noncanonical");
  ExpectRejected("606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f",
                 SecurityError::kInvalidPublicKey,
                 "encoding without square root is rejected");

  const Bytes short_key(31, 0);
  const auto short_result = ValidateEd25519PublicKey(short_key);
  Expect(!short_result.ok() && short_result.error == SecurityError::kInvalidLength,
         "non-32-byte key is rejected before decoding");
}

void TestIdentityRepositoryAdapter() {
  OpenSslPeerPublicKeyValidator validator;
  const Bytes valid_bytes =
      DecodeHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  PublicKey valid{};
  std::copy(valid_bytes.begin(), valid_bytes.end(), valid.begin());
  Expect(validator.Validate(valid).ok(), "identity adapter accepts prime-subgroup key");

  PublicKey identity{};
  identity[0] = 1;
  const auto rejected = validator.Validate(identity);
  Expect(!rejected.ok() && rejected.error() == ErrorCode::kInvalidArgument,
         "identity adapter rejects hostile key fail-closed");
}

void TestPrimeSubgroupOrderUsesFixedLoop() {
  Expect(xnn_transfer::core::security::tls::internal::
                 PrimeSubgroupOrderLoopCountForTesting() == 256,
         "prime-subgroup multiplication uses fixed 256-bit loop");
}

}  // namespace

int main() {
  TestPublishedPrimeOrderKeys();
  TestOpenSslDerivedPublicKeys();
  TestHostileSubgroupKeys();
  TestYCoordinateAndSignBoundaries();
  TestNonCanonicalAndInvalidEncodings();
  TestIdentityRepositoryAdapter();
  TestPrimeSubgroupOrderUsesFixedLoop();

  if (failures != 0) {
    std::cerr << failures << " Ed25519 public-key test(s) failed\n";
    return 1;
  }
  std::cout << "Ed25519 canonical and prime-subgroup validation passed\n";
  return 0;
}
