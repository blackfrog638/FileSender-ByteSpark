add_executable(
  xnn_transfer_identity_tests
  identity_repository_test.cpp
)

target_link_libraries(
  xnn_transfer_identity_tests
  PRIVATE
    xnn_transfer_identity
)

if(MSVC)
  target_compile_options(
    xnn_transfer_identity_tests
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_identity_tests
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_identity_tests)
endif()

add_test(
  NAME xnn_transfer_identity_tests
  COMMAND xnn_transfer_identity_tests
)
