add_library(
  xnn_transfer_identity
  STATIC
  codec.cpp
  crypto.cpp
  identity_repository.cpp
  platform_protected_store.cpp
  secret_buffer.cpp
  types.cpp
)

target_compile_features(xnn_transfer_identity PUBLIC cxx_std_20)
set_target_properties(
  xnn_transfer_identity
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
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

if(UNIX)
  target_sources(
    xnn_transfer_identity
    PRIVATE
      posix_store_lock.cpp
  )
endif()

if(APPLE)
  find_library(
    identity_core_foundation_framework
    CoreFoundation
    REQUIRED
  )
  find_library(
    identity_security_framework
    Security
    REQUIRED
  )
  target_sources(
    xnn_transfer_identity
    PRIVATE
      macos_keychain_backend.cpp
  )
  target_link_libraries(
    xnn_transfer_identity
    PRIVATE
      "${identity_core_foundation_framework}"
      "${identity_security_framework}"
  )
endif()

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

if(WIN32)
  target_sources(
    xnn_transfer_identity
    PRIVATE
      windows_credential_backend.cpp
  )
  target_link_libraries(
    xnn_transfer_identity
    PRIVATE
      Advapi32
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

if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  target_compile_options(
    xnn_transfer_identity
    PRIVATE
      -Wno-elaborated-enum-base
      -Wno-nullability-extension
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_identity)
endif()
