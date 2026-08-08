find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(
  xnn_transfer_storage_manifest_vectors
  "${CMAKE_CURRENT_BINARY_DIR}/manifest_fixture_cases.inc"
)
add_custom_command(
  OUTPUT "${xnn_transfer_storage_manifest_vectors}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_manifest_fixture_cases.py"
    --input
    "${PROJECT_SOURCE_DIR}/protocol/testdata/v1/manifest/vectors.json"
    --output
    "${xnn_transfer_storage_manifest_vectors}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_manifest_fixture_cases.py"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/v1/manifest/vectors.json"
  VERBATIM
)

add_executable(
  xnn_transfer_storage_tests
  storage_test.cpp
  "${xnn_transfer_storage_manifest_vectors}"
)
target_include_directories(
  xnn_transfer_storage_tests
  PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(
  xnn_transfer_storage_tests
  PRIVATE
    Threads::Threads
    xnn_transfer_storage
)
if(WIN32)
  target_link_libraries(
    xnn_transfer_storage_tests
    PRIVATE
      Advapi32
  )
endif()

if(MSVC)
  target_compile_options(
    xnn_transfer_storage_tests
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_storage_tests
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_storage_tests)
endif()

add_test(
  NAME xnn_transfer_storage_tests
  COMMAND xnn_transfer_storage_tests
)
set_tests_properties(
  xnn_transfer_storage_tests
  PROPERTIES
    TIMEOUT 30
)

add_test(
  NAME xnn_transfer_storage_fixture_classification
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_manifest_fixture_cases.py"
    --input
    "${PROJECT_SOURCE_DIR}/protocol/testdata/v1/manifest/vectors.json"
    --check
)
