add_executable(
  xnn_transfer_protocol_tests
  v1_parser_test.cpp
)
target_link_libraries(xnn_transfer_protocol_tests PRIVATE xnn_transfer_protocol)

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_protocol_tests)
endif()

add_test(NAME xnn_transfer_protocol_tests COMMAND xnn_transfer_protocol_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME xnn_transfer_protocol_vector_oracle
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/v1/validate_vectors.py"
)
add_test(
  NAME xnn_transfer_protocol_vector_drift
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/v1/generate_native_vectors.py"
    --check
)
add_test(
  NAME xnn_transfer_manifest_vector_oracle
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/v1/manifest/validate_vectors.py"
)
add_test(
  NAME xnn_transfer_security_profile_vector_oracle
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/validate_vectors.py"
)
add_test(
  NAME xnn_transfer_security_wordlist_line_endings
  COMMAND
    "${Python3_EXECUTABLE}"
    "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/test_wordlist_line_endings.py"
)
