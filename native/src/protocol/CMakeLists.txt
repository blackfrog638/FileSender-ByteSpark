add_library(
  xnn_transfer_protocol
  STATIC
  v1_parser.cpp
)

target_compile_features(xnn_transfer_protocol PUBLIC cxx_std_20)
target_include_directories(
  xnn_transfer_protocol
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    "$<INSTALL_INTERFACE:include>"
)
set_target_properties(
  xnn_transfer_protocol
  PROPERTIES
    POSITION_INDEPENDENT_CODE YES
)

if(MSVC)
  target_compile_options(xnn_transfer_protocol PRIVATE /W4 /permissive-)
else()
  target_compile_options(
    xnn_transfer_protocol
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN OR XNN_TRANSFER_BUILD_FUZZERS)
  xnn_transfer_enable_sanitizers(xnn_transfer_protocol)
endif()
