add_library(
  xnn_transfer_tls
  STATIC
  canonical_codec.cpp
  confirmation.cpp
  contexts.cpp
  crypto_primitives.cpp
  ed25519_public_key.cpp
  identity.cpp
  rotation.cpp
  tls_provider.cpp
)

target_compile_features(xnn_transfer_tls PUBLIC cxx_std_20)
set_target_properties(
  xnn_transfer_tls
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(
  xnn_transfer_tls
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_tls
  PUBLIC
    xnn_transfer_identity
  PRIVATE
    OpenSSL::Crypto
    OpenSSL::SSL
)

if(MSVC)
  target_compile_options(
    xnn_transfer_tls
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_tls
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_tls)
endif()
