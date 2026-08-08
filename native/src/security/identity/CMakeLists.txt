add_library(
  xnn_transfer_identity
  STATIC
  codec.cpp
  crypto.cpp
  identity_repository.cpp
  linux_secret_service_store.cpp
  secret_buffer.cpp
  types.cpp
)

target_include_directories(
  xnn_transfer_identity
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_identity
  PRIVATE
    OpenSSL::Crypto
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(
    xnn_transfer_identity
    PRIVATE
      linux_secret_service_backend.cpp
  )
  target_link_libraries(
    xnn_transfer_identity
    PRIVATE
      XnnDependencies::libsecret
  )
endif()

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
