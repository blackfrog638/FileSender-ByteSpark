#include "xnn_transfer/core/security/identity/secret_buffer.hpp"

#include <openssl/crypto.h>

#include <utility>

namespace xnn_transfer::core::security::identity {

SecretBuffer::SecretBuffer(const std::size_t size) : bytes_(size) {}

SecretBuffer::SecretBuffer(const std::span<const std::uint8_t> bytes)
    : bytes_(bytes.begin(), bytes.end()) {}

SecretBuffer::~SecretBuffer() { clear(); }

SecretBuffer::SecretBuffer(SecretBuffer&& other) noexcept
    : bytes_(std::move(other.bytes_)) {
  other.clear();
}

SecretBuffer& SecretBuffer::operator=(SecretBuffer&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  clear();
  bytes_ = std::move(other.bytes_);
  other.clear();
  return *this;
}

std::span<const std::uint8_t> SecretBuffer::bytes() const noexcept { return bytes_; }

std::span<std::uint8_t> SecretBuffer::mutable_bytes() noexcept { return bytes_; }

std::size_t SecretBuffer::size() const noexcept { return bytes_.size(); }

bool SecretBuffer::empty() const noexcept { return bytes_.empty(); }

void SecretBuffer::clear() noexcept {
  if (!bytes_.empty()) {
    OPENSSL_cleanse(bytes_.data(), bytes_.size());
    bytes_.clear();
  }
}

}  // namespace xnn_transfer::core::security::identity
