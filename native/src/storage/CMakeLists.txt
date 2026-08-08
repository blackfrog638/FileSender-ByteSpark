add_library(
  xnn_transfer_storage
  STATIC
  filesystem_backend.cpp
  path_validator.cpp
  receive_transaction.cpp
)

target_include_directories(
  xnn_transfer_storage
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/native/include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_storage
  PRIVATE
    utf8proc::utf8proc
)

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
