add_library(
  xnn_transfer_identity
  STATIC
  codec.cpp
  crypto.cpp
  identity_repository.cpp
  secret_buffer.cpp
  types.cpp
)

target_include_directories(
  xnn_transfer_identity
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/native/include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_identity
  PRIVATE
    OpenSSL::Crypto
)

if(MSVC)
  target_compile_options(
    xnn_transfer_identity
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_identity
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_identity)
endif()
