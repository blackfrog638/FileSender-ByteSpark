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

add_executable(
  xnn_transfer_linux_secret_service_store_tests
  linux_secret_service_store_test.cpp
)

target_include_directories(
  xnn_transfer_linux_secret_service_store_tests
  PRIVATE
    "${PROJECT_SOURCE_DIR}/native/src/security/identity"
)

target_link_libraries(
  xnn_transfer_linux_secret_service_store_tests
  PRIVATE
    Threads::Threads
    xnn_transfer_identity
)

if(MSVC)
  target_compile_options(
    xnn_transfer_linux_secret_service_store_tests
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_linux_secret_service_store_tests
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
    xnn_transfer_linux_secret_service_store_tests
  )
endif()

add_test(
  NAME xnn_transfer_linux_secret_service_store_tests
  COMMAND xnn_transfer_linux_secret_service_store_tests
)
