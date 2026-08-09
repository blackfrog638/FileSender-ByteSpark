find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(
  xnn_transfer_transfer_pairing_vectors
  "${CMAKE_CURRENT_BINARY_DIR}/pairing_vectors.hpp"
)
add_custom_command(
  OUTPUT "${xnn_transfer_transfer_pairing_vectors}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/native/tests/session/generate_pairing_vectors.py"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/pairing-control-vectors.json"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/vectors.json"
    "${xnn_transfer_transfer_pairing_vectors}"
  DEPENDS
    "${PROJECT_SOURCE_DIR}/native/tests/session/generate_pairing_vectors.py"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/pairing-control-vectors.json"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/vectors.json"
  VERBATIM
)

add_executable(
  xnn_transfer_one_file_transfer_tests
  one_file_transfer_test.cpp
  "${xnn_transfer_transfer_pairing_vectors}"
)
target_include_directories(
  xnn_transfer_one_file_transfer_tests
  PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(
  xnn_transfer_one_file_transfer_tests
  PRIVATE
    OpenSSL::Crypto
    OpenSSL::SSL
    Threads::Threads
    xnn_transfer_identity
    xnn_transfer_tls
    xnn_transfer_transfer
)

if(MSVC)
  target_compile_options(
    xnn_transfer_one_file_transfer_tests
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_one_file_transfer_tests
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(
    xnn_transfer_one_file_transfer_tests
  )
endif()

add_test(
  NAME xnn_transfer_one_file_transfer_tests
  COMMAND xnn_transfer_one_file_transfer_tests
)
set_tests_properties(
  xnn_transfer_one_file_transfer_tests
  PROPERTIES
    TIMEOUT 120
)
