#include "xnn_transfer/core/security/identity/crypto.hpp"

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <memory>
#include <new>
#include <string_view>

namespace xnn_transfer::core::security::identity {
namespace {

constexpr std::string_view kDeviceIdLabel = "XnnTransfer device identifier v1";
constexpr std::string_view kPeerMacKeySalt = "XnnTransfer identity root HKDF salt v1";
constexpr std::string_view kPeerMacKeyInfo = "XnnTransfer peer record MAC key v1";
static_assert(kDeviceIdLabel.size() == 32);

void WriteU16(std::span<std::uint8_t> output, const std::size_t offset,
              const std::uint16_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteU32(std::span<std::uint8_t> output, const std::size_t offset,
              const std::uint32_t value) {
  output[offset] = static_cast<std::uint8_t>(value >> 24U);
  output[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
  output[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
  output[offset + 3] = static_cast<std::uint8_t>(value);
}

}  // namespace

Result<SecretBuffer> OpenSslIdentityCrypto::GenerateSeed() {
  try {
    SecretBuffer seed(kEd25519SeedSize);
    if (RAND_priv_bytes(seed.mutable_bytes().data(), static_cast<int>(seed.size())) !=
        1) {
      return Result<SecretBuffer>::Failure(ErrorCode::kEntropyFailure);
    }
    return Result<SecretBuffer>::Success(std::move(seed));
  } catch (const std::bad_alloc&) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
  }
}

Result<StoreId> OpenSslIdentityCrypto::GenerateStoreId() {
  StoreId identifier{};
  if (RAND_priv_bytes(identifier.data(), static_cast<int>(identifier.size())) != 1) {
    return Result<StoreId>::Failure(ErrorCode::kEntropyFailure);
  }
  return Result<StoreId>::Success(identifier);
}

Result<PublicKey> OpenSslIdentityCrypto::DerivePublicKey(
    const std::span<const std::uint8_t> seed) {
  if (seed.size() != kEd25519SeedSize) {
    return Result<PublicKey>::Failure(ErrorCode::kInvalidArgument);
  }

  using KeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
  KeyPtr key(EVP_PKEY_new_raw_private_key_ex(nullptr, "ED25519", nullptr, seed.data(),
                                             seed.size()),
             EVP_PKEY_free);
  if (!key) {
    return Result<PublicKey>::Failure(ErrorCode::kCryptoFailure);
  }

  PublicKey public_key{};
  std::size_t output_size = public_key.size();
  if (EVP_PKEY_get_raw_public_key(key.get(), public_key.data(), &output_size) != 1 ||
      output_size != public_key.size()) {
    return Result<PublicKey>::Failure(ErrorCode::kCryptoFailure);
  }
  return Result<PublicKey>::Success(public_key);
}

Result<DeviceId> OpenSslIdentityCrypto::DeriveDeviceId(const PublicKey& public_key) {
  constexpr std::size_t kEnvelopeSize = 12;
  constexpr std::size_t kFieldHeaderSize = 6;
  constexpr std::size_t kBodySize =
      (2 * kFieldHeaderSize) + kDeviceIdLabel.size() + kEd25519PublicKeySize;
  std::array<std::uint8_t, kEnvelopeSize + kBodySize> encoded{};

  std::copy_n("XNNS", 4, encoded.begin());
  encoded[4] = 1;
  encoded[5] = 7;
  WriteU16(encoded, 6, 2);
  WriteU32(encoded, 8, static_cast<std::uint32_t>(kBodySize));

  std::size_t offset = kEnvelopeSize;
  WriteU16(encoded, offset, 1);
  WriteU32(encoded, offset + 2, static_cast<std::uint32_t>(kDeviceIdLabel.size()));
  offset += kFieldHeaderSize;
  std::copy(kDeviceIdLabel.begin(), kDeviceIdLabel.end(),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset));
  offset += kDeviceIdLabel.size();

  WriteU16(encoded, offset, 2);
  WriteU32(encoded, offset + 2, static_cast<std::uint32_t>(public_key.size()));
  offset += kFieldHeaderSize;
  std::copy(public_key.begin(), public_key.end(),
            encoded.begin() + static_cast<std::ptrdiff_t>(offset));

  DeviceId identifier{};
  unsigned int output_size = 0;
  if (EVP_Digest(encoded.data(), encoded.size(), identifier.data(), &output_size,
                 EVP_sha256(), nullptr) != 1 ||
      output_size != identifier.size()) {
    return Result<DeviceId>::Failure(ErrorCode::kCryptoFailure);
  }
  return Result<DeviceId>::Success(identifier);
}

Result<SecretBuffer> OpenSslIdentityCrypto::DerivePeerRecordMacKey(
    const std::span<const std::uint8_t> seed, const StoreId& store_id) {
  if (seed.size() != kEd25519SeedSize) {
    return Result<SecretBuffer>::Failure(ErrorCode::kInvalidArgument);
  }

  using KdfPtr = std::unique_ptr<EVP_KDF, decltype(&EVP_KDF_free)>;
  using KdfContextPtr = std::unique_ptr<EVP_KDF_CTX, decltype(&EVP_KDF_CTX_free)>;
  KdfPtr algorithm(EVP_KDF_fetch(nullptr, "HKDF", nullptr), EVP_KDF_free);
  if (!algorithm) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
  }
  KdfContextPtr context(EVP_KDF_CTX_new(algorithm.get()), EVP_KDF_CTX_free);
  if (!context) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
  }

  std::array<std::uint8_t, kPeerMacKeyInfo.size() + kStoreIdSize> info{};
  std::copy(kPeerMacKeyInfo.begin(), kPeerMacKeyInfo.end(), info.begin());
  std::copy(store_id.begin(), store_id.end(),
            info.begin() + static_cast<std::ptrdiff_t>(kPeerMacKeyInfo.size()));
  char digest_name[] = "SHA256";
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest_name, 0),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_KEY, const_cast<std::uint8_t*>(seed.data()), seed.size()),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT,
                                        const_cast<char*>(kPeerMacKeySalt.data()),
                                        kPeerMacKeySalt.size()),
      OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, info.data(), info.size()),
      OSSL_PARAM_construct_end(),
  };

  try {
    SecretBuffer output(kMacSize);
    if (EVP_KDF_derive(context.get(), output.mutable_bytes().data(), output.size(),
                       parameters) != 1) {
      return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
    }
    return Result<SecretBuffer>::Success(std::move(output));
  } catch (const std::bad_alloc&) {
    return Result<SecretBuffer>::Failure(ErrorCode::kCryptoFailure);
  }
}

Result<Mac> OpenSslIdentityCrypto::HmacSha256(
    const std::span<const std::uint8_t> key,
    const std::span<const std::uint8_t> message) {
  if (key.empty()) {
    return Result<Mac>::Failure(ErrorCode::kInvalidArgument);
  }

  using MacPtr = std::unique_ptr<EVP_MAC, decltype(&EVP_MAC_free)>;
  using MacContextPtr = std::unique_ptr<EVP_MAC_CTX, decltype(&EVP_MAC_CTX_free)>;
  MacPtr algorithm(EVP_MAC_fetch(nullptr, "HMAC", nullptr), EVP_MAC_free);
  if (!algorithm) {
    return Result<Mac>::Failure(ErrorCode::kCryptoFailure);
  }
  MacContextPtr context(EVP_MAC_CTX_new(algorithm.get()), EVP_MAC_CTX_free);
  if (!context) {
    return Result<Mac>::Failure(ErrorCode::kCryptoFailure);
  }

  char digest_name[] = "SHA256";
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0),
      OSSL_PARAM_construct_end(),
  };
  if (EVP_MAC_init(context.get(), key.data(), key.size(), parameters) != 1 ||
      EVP_MAC_update(context.get(), message.data(), message.size()) != 1) {
    return Result<Mac>::Failure(ErrorCode::kCryptoFailure);
  }

  Mac output{};
  std::size_t output_size = 0;
  if (EVP_MAC_final(context.get(), output.data(), &output_size, output.size()) != 1 ||
      output_size != output.size()) {
    return Result<Mac>::Failure(ErrorCode::kCryptoFailure);
  }
  return Result<Mac>::Success(output);
}

}  // namespace xnn_transfer::core::security::identity
