find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(
  xnn_transfer_pairing_vectors
  "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/pairing-control-vectors.json"
)
set(
  xnn_transfer_security_vectors
  "${PROJECT_SOURCE_DIR}/protocol/testdata/security/v1/vectors.json"
)
set(
  xnn_transfer_generated_pairing_vectors
  "${CMAKE_CURRENT_BINARY_DIR}/pairing_vectors.hpp"
)

add_custom_command(
  OUTPUT "${xnn_transfer_generated_pairing_vectors}"
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_pairing_vectors.py"
    "${xnn_transfer_pairing_vectors}"
    "${xnn_transfer_security_vectors}"
    "${xnn_transfer_generated_pairing_vectors}"
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generate_pairing_vectors.py"
    "${xnn_transfer_pairing_vectors}"
    "${xnn_transfer_security_vectors}"
  VERBATIM
)

function(xnn_transfer_configure_session_test target)
  target_link_libraries(
    "${target}"
    PRIVATE
      OpenSSL::Crypto
      OpenSSL::SSL
      xnn_transfer_identity
      xnn_transfer_session
      xnn_transfer_tls
  )
  target_include_directories(
    "${target}"
    PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}"
  )

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

  add_test(NAME "${target}" COMMAND "${target}")
  set_tests_properties("${target}" PROPERTIES TIMEOUT 120)
endfunction()

add_executable(
  xnn_transfer_pairing_session_tests
  pairing_session_test.cpp
  "${xnn_transfer_generated_pairing_vectors}"
)
xnn_transfer_configure_session_test(xnn_transfer_pairing_session_tests)

add_executable(
  xnn_transfer_session_authority_tests
  session_authority_test.cpp
  "${xnn_transfer_generated_pairing_vectors}"
)
xnn_transfer_configure_session_test(xnn_transfer_session_authority_tests)
