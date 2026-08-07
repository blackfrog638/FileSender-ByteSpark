add_executable(
  xnn_transfer_discovery_tests
  discovery_test.cpp
)
target_link_libraries(
  xnn_transfer_discovery_tests
  PRIVATE
    Threads::Threads
    xnn_transfer_discovery
)
if(MSVC)
  target_compile_options(
    xnn_transfer_discovery_tests
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_discovery_tests
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_discovery_tests)
endif()

add_test(
  NAME xnn_transfer_discovery_tests
  COMMAND xnn_transfer_discovery_tests
)

add_executable(
  xnn_transfer_discovery_runtime_tests
  runtime_test.cpp
)
target_include_directories(
  xnn_transfer_discovery_runtime_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/native/src/discovery"
)
target_compile_definitions(
  xnn_transfer_discovery_runtime_tests
  PRIVATE
    ASIO_NO_DEPRECATED
    ASIO_STANDALONE
)
if(WIN32)
  target_compile_definitions(
    xnn_transfer_discovery_runtime_tests
    PRIVATE
      NTDDI_VERSION=0x06010000
      WINVER=0x0601
      _WIN32_WINNT=0x0601
  )
endif()
target_link_libraries(
  xnn_transfer_discovery_runtime_tests
  PRIVATE
    Threads::Threads
    asio::asio
    xnn_transfer_discovery
)
if(MSVC)
  target_compile_options(
    xnn_transfer_discovery_runtime_tests
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_discovery_runtime_tests
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_discovery_runtime_tests)
endif()

add_test(
  NAME xnn_transfer_discovery_runtime_tests
  COMMAND xnn_transfer_discovery_runtime_tests
)
set_tests_properties(
  xnn_transfer_discovery_runtime_tests
  PROPERTIES
    TIMEOUT 30
)
