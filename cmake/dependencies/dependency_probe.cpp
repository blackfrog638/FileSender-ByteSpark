#include <openssl/opensslv.h>
#include <openssl/ssl.h>
#include <utf8proc.h>

#include <asio/io_context.hpp>
#include <asio/version.hpp>
#include <cstring>
#include <iostream>
#include <memory>
#include <string_view>

namespace {

constexpr int kExpectedAsioVersion = 103'802;
constexpr std::string_view kExpectedOpenSslPrefix = "OpenSSL 3.5.7 ";
constexpr std::string_view kExpectedUtf8procVersion = "2.11.3";

}  // namespace

int main() {
  static_assert(ASIO_VERSION == kExpectedAsioVersion);

  const std::string_view openssl_version = OPENSSL_VERSION_TEXT;
  if (!openssl_version.starts_with(kExpectedOpenSslPrefix)) {
    std::cerr << "Unexpected OpenSSL version: " << openssl_version << '\n';
    return 1;
  }
  if (std::string_view(utf8proc_version()) != kExpectedUtf8procVersion) {
    std::cerr << "Unexpected utf8proc version: " << utf8proc_version() << '\n';
    return 1;
  }

  asio::io_context context;
  using SslContext = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
  SslContext ssl_context(SSL_CTX_new(TLS_method()), &SSL_CTX_free);
  if (ssl_context == nullptr || context.stopped()) {
    std::cerr << "Pinned dependency initialization failed.\n";
    return 1;
  }

  std::cout << "Asio 1.38.2, " << openssl_version << ", utf8proc " << utf8proc_version()
            << '\n';
  return 0;
}
