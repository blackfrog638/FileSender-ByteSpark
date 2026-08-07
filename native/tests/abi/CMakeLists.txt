find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(xnn_transfer_check_abi_fixture name expected_to_compile)
  unset(fixture_compiled CACHE)
  try_compile(
    fixture_compiled
    "${CMAKE_CURRENT_BINARY_DIR}/compile-fixtures/${name}"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/${name}.cpp"
    CMAKE_FLAGS
      "-DCMAKE_CXX_STANDARD=20"
      "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
      "-DCMAKE_CXX_EXTENSIONS=OFF"
    OUTPUT_VARIABLE fixture_output
  )
  set(fixture_result "${fixture_compiled}")
  unset(fixture_compiled CACHE)

  if(expected_to_compile AND NOT fixture_result)
    message(
      FATAL_ERROR
      "Additive ABI fixture ${name} failed to compile:\n${fixture_output}"
    )
  endif()
  if(NOT expected_to_compile AND fixture_result)
    message(FATAL_ERROR "Breaking ABI fixture ${name} compiled successfully")
  endif()
  if(NOT expected_to_compile)
    string(
      FIND
      "${fixture_output}"
      "v1_compat_assertions.hpp"
      assertion_failure
    )
    if(assertion_failure EQUAL -1)
      message(
        FATAL_ERROR
        "Breaking ABI fixture ${name} failed outside the shared assertions:\n"
        "${fixture_output}"
      )
    endif()
    message(STATUS "ABI compile fixture ${name}: rejected as expected")
  else()
    message(STATUS "ABI compile fixture ${name}: accepted as expected")
  endif()
endfunction()

xnn_transfer_check_abi_fixture(additive_extension TRUE)
xnn_transfer_check_abi_fixture(breaking_macro_value FALSE)
xnn_transfer_check_abi_fixture(breaking_struct_offset FALSE)
xnn_transfer_check_abi_fixture(breaking_field_type FALSE)
xnn_transfer_check_abi_fixture(breaking_function_signature FALSE)

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
set(abi_runtime_tests
    xnn_transfer_abi_current_layout
    xnn_transfer_abi_v1_frozen_layout
    xnn_transfer_abi_v1_legacy_client
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
  list(APPEND abi_runtime_tests xnn_transfer_abi_v1_exports)
endif()

set_tests_properties(${abi_runtime_tests} PROPERTIES TIMEOUT 30)
