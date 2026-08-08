#ifndef XNN_TRANSFER_CORE_SECURITY_IDENTITY_SECRET_BUFFER_HPP_
#define XNN_TRANSFER_CORE_SECURITY_IDENTITY_SECRET_BUFFER_HPP_

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace xnn_transfer::core::security::identity {

class SecretBuffer final {
 public:
  SecretBuffer() = default;
  explicit SecretBuffer(std::size_t size);
  explicit SecretBuffer(std::span<const std::uint8_t> bytes);
  ~SecretBuffer();

  SecretBuffer(const SecretBuffer&) = delete;
  SecretBuffer& operator=(const SecretBuffer&) = delete;

  SecretBuffer(SecretBuffer&& other) noexcept;
  SecretBuffer& operator=(SecretBuffer&& other) noexcept;

  [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept;
  [[nodiscard]] std::span<std::uint8_t> mutable_bytes() noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;

  void clear() noexcept;

 private:
  std::vector<std::uint8_t> bytes_;
};

}  // namespace xnn_transfer::core::security::identity

#endif  // XNN_TRANSFER_CORE_SECURITY_IDENTITY_SECRET_BUFFER_HPP_
