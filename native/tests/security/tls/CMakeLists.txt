function(xnn_transfer_configure_tls_test target)
  if(MSVC)
    target_compile_options(
      "${target}"
      PRIVATE
        /W4
        /permissive-
    )
  else()
    target_compile_options(
      "${target}"
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
  endif()

  if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
    xnn_transfer_enable_sanitizers("${target}")
  endif()
endfunction()

add_executable(
  xnn_transfer_ed25519_public_key_tests
  ed25519_public_key_test.cpp
)
target_link_libraries(
  xnn_transfer_ed25519_public_key_tests
  PRIVATE
    OpenSSL::Crypto
    xnn_transfer_tls
)
xnn_transfer_configure_tls_test(
  xnn_transfer_ed25519_public_key_tests
)
add_test(
  NAME xnn_transfer_ed25519_public_key_tests
  COMMAND xnn_transfer_ed25519_public_key_tests
)
set_tests_properties(
  xnn_transfer_ed25519_public_key_tests
  PROPERTIES
    TIMEOUT 120
)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(
  xnn_transfer_security_vectors
  "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/vectors.json"
)
set(
  xnn_transfer_security_wordlist
  "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/wordlist.txt"
)
set(
  xnn_transfer_generated_security_vectors
  "${CMAKE_CURRENT_BINARY_DIR}/security_v1_golden_vectors.hpp"
)

add_custom_command(
  OUTPUT "${xnn_transfer_generated_security_vectors}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_golden_vectors.py"
    "${xnn_transfer_security_vectors}"
    "${xnn_transfer_generated_security_vectors}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_golden_vectors.py"
    "${xnn_transfer_security_vectors}"
    "${xnn_transfer_security_wordlist}"
  VERBATIM
)

add_executable(
  xnn_transfer_security_profile_tests
  profile_test.cpp
  "${xnn_transfer_generated_security_vectors}"
)
target_include_directories(
  xnn_transfer_security_profile_tests
  PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(
  xnn_transfer_security_profile_tests
  PRIVATE
    xnn_transfer_tls
)
xnn_transfer_configure_tls_test(
  xnn_transfer_security_profile_tests
)
add_test(
  NAME xnn_transfer_security_profile_tests
  COMMAND xnn_transfer_security_profile_tests
)
set_tests_properties(
  xnn_transfer_security_profile_tests
  PROPERTIES
    TIMEOUT 60
)

add_executable(
  xnn_transfer_tls_provider_tests
  tls_provider_test.cpp
)
target_link_libraries(
  xnn_transfer_tls_provider_tests
  PRIVATE
    OpenSSL::Crypto
    OpenSSL::SSL
    xnn_transfer_tls
)
xnn_transfer_configure_tls_test(
  xnn_transfer_tls_provider_tests
)
add_test(
  NAME xnn_transfer_tls_provider_tests
  COMMAND xnn_transfer_tls_provider_tests
)
set_tests_properties(
  xnn_transfer_tls_provider_tests
  PROPERTIES
    TIMEOUT 60
)
