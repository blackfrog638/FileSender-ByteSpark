add_library(
  xnn_transfer_storage
  STATIC
  filesystem_backend.cpp
  path_validator.cpp
  receive_transaction.cpp
)
if(WIN32)
  target_sources(
    xnn_transfer_storage
    PRIVATE
      filesystem_backend_windows.cpp
  )
endif()

target_compile_features(xnn_transfer_storage PUBLIC cxx_std_20)
set_target_properties(
  xnn_transfer_storage
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(
  xnn_transfer_storage
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_storage
  PRIVATE
    utf8proc::utf8proc
)
if(WIN32)
  target_link_libraries(
    xnn_transfer_storage
    PRIVATE
      Advapi32
      Bcrypt
  )
endif()

if(MSVC)
  target_compile_options(
    xnn_transfer_storage
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_storage
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_storage)
endif()
