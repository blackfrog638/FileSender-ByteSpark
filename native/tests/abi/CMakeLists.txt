find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(
  xnn_transfer_abi_current_layout_test
  current_layout_test.cpp
)
target_link_libraries(
  xnn_transfer_abi_current_layout_test
  PRIVATE xnn_transfer_core
)

add_executable(
  xnn_transfer_abi_v1_frozen_layout_test
  v1_frozen_layout_test.cpp
)
target_include_directories(
  xnn_transfer_abi_v1_frozen_layout_test
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

add_executable(
  xnn_transfer_abi_v1_legacy_client
  v1_legacy_client.cpp
)
target_include_directories(
  xnn_transfer_abi_v1_legacy_client
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
  xnn_transfer_abi_v1_legacy_client
  PRIVATE xnn_transfer_core
)

foreach(
  target
  IN ITEMS
    xnn_transfer_abi_current_layout_test
    xnn_transfer_abi_v1_frozen_layout_test
    xnn_transfer_abi_v1_legacy_client
)
  target_compile_features(${target} PRIVATE cxx_std_20)
  if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
    xnn_transfer_enable_sanitizers(${target})
  endif()
endforeach()

add_test(
  NAME xnn_transfer_abi_current_layout
  COMMAND xnn_transfer_abi_current_layout_test
)
add_test(
  NAME xnn_transfer_abi_v1_frozen_layout
  COMMAND xnn_transfer_abi_v1_frozen_layout_test
)
add_test(
  NAME xnn_transfer_abi_v1_legacy_client
  COMMAND xnn_transfer_abi_v1_legacy_client
)

if(NOT XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  add_test(
    NAME xnn_transfer_abi_v1_exports
    COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/export_test.py"
      "$<TARGET_FILE:xnn_transfer_core>"
      "${CMAKE_CURRENT_SOURCE_DIR}/v1_required_exports.txt"
  )
endif()

add_test(
  NAME xnn_transfer_abi_v1_compile_fixtures
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/negative_compile_test.py"
    --header
    "${CMAKE_CURRENT_SOURCE_DIR}/v1/c_api.h"
    --assertions
    "${CMAKE_CURRENT_SOURCE_DIR}/v1_compat_assertions.hpp"
    --cmake
    "${CMAKE_COMMAND}"
    --generator
    "${CMAKE_GENERATOR}"
)
set_tests_properties(
  xnn_transfer_abi_v1_compile_fixtures
  PROPERTIES TIMEOUT 120
)
