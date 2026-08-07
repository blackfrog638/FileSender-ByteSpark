if(APPLE)
  set(xnn_transfer_discovery_platform_source interface_monitor_macos.cpp)
elseif(WIN32)
  set(xnn_transfer_discovery_platform_source interface_monitor_windows.cpp)
elseif(UNIX)
  set(xnn_transfer_discovery_platform_source interface_monitor_linux.cpp)
else()
  message(FATAL_ERROR "Unsupported discovery platform")
endif()

add_library(
  xnn_transfer_discovery
  STATIC
  asio_transport.cpp
  cache.cpp
  interface_registry.cpp
  "${xnn_transfer_discovery_platform_source}"
  service.cpp
  system_runtime.cpp
  utf8proc_label_validator.cpp
  wire.cpp
)

target_compile_features(xnn_transfer_discovery PUBLIC cxx_std_20)
set_target_properties(
  xnn_transfer_discovery
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(
  xnn_transfer_discovery
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_discovery
  PRIVATE
    asio::asio
    OpenSSL::Crypto
    utf8proc::utf8proc
)

target_compile_definitions(
  xnn_transfer_discovery
  PRIVATE
    ASIO_NO_DEPRECATED
    ASIO_STANDALONE
)

if(APPLE)
  target_compile_definitions(
    xnn_transfer_discovery
    PRIVATE
      __APPLE_USE_RFC_3542
  )
  find_library(
    discovery_system_configuration_framework
    SystemConfiguration
    REQUIRED
  )
  find_library(
    discovery_core_foundation_framework
    CoreFoundation
    REQUIRED
  )
  target_link_libraries(
    xnn_transfer_discovery
    PRIVATE
      "${discovery_core_foundation_framework}"
      "${discovery_system_configuration_framework}"
  )
elseif(WIN32)
  target_compile_definitions(
    xnn_transfer_discovery
    PRIVATE
      NTDDI_VERSION=0x06010000
      WINVER=0x0601
      _WIN32_WINNT=0x0601
  )
  target_link_libraries(
    xnn_transfer_discovery
    PRIVATE
      Iphlpapi
      Ws2_32
  )
endif()

if(MSVC)
  target_compile_options(
    xnn_transfer_discovery
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_discovery
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
    xnn_transfer_discovery
    PRIVATE
      -Wno-elaborated-enum-base
      -Wno-nullability-extension
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_discovery)
endif()
