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
  xnn_transfer_platform_protected_store_tests
  platform_protected_store_test.cpp
)

target_include_directories(
  xnn_transfer_platform_protected_store_tests
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/security/identity"
)

target_link_libraries(
  xnn_transfer_platform_protected_store_tests
  PRIVATE
    Threads::Threads
    xnn_transfer_identity
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(
    xnn_transfer_platform_protected_store_tests
    PRIVATE
      XnnDependencies::libsecret
  )
endif()

if(MSVC)
  target_compile_options(
    xnn_transfer_platform_protected_store_tests
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_platform_protected_store_tests
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
    xnn_transfer_platform_protected_store_tests
  )
endif()

add_test(
  NAME xnn_transfer_platform_protected_store_tests
  COMMAND xnn_transfer_platform_protected_store_tests
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_program(
    dbus_run_session_executable
    NAMES dbus-run-session
    REQUIRED
  )
  find_program(
    gnome_keyring_daemon_executable
    NAMES gnome-keyring-daemon
    REQUIRED
  )
  file(
    REAL_PATH
    "${gnome_keyring_daemon_executable}"
    qualified_gnome_keyring_daemon
  )
  if(NOT qualified_gnome_keyring_daemon STREQUAL
     "/usr/bin/gnome-keyring-daemon")
    message(
      FATAL_ERROR
      "GNOME Keyring qualification requires /usr/bin/gnome-keyring-daemon, "
      "found ${qualified_gnome_keyring_daemon}"
    )
  endif()

  add_test(
    NAME xnn_transfer_linux_secret_service_integration_tests
    COMMAND
      "${dbus_run_session_executable}"
      --
      "$<TARGET_FILE:xnn_transfer_platform_protected_store_tests>"
      --linux-secret-service-integration
      "${qualified_gnome_keyring_daemon}"
  )
  set_tests_properties(
    xnn_transfer_linux_secret_service_integration_tests
    PROPERTIES
      RUN_SERIAL TRUE
      TIMEOUT 180
  )
endif()
