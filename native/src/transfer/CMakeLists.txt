add_library(
  xnn_transfer_transfer
  STATIC
  frame_codec.cpp
  receiver.cpp
  sender.cpp
)

target_compile_features(xnn_transfer_transfer PUBLIC cxx_std_20)
set_target_properties(
  xnn_transfer_transfer
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(
  xnn_transfer_transfer
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_transfer
  PUBLIC
    xnn_transfer_protocol
    xnn_transfer_session
    xnn_transfer_storage
)

if(MSVC)
  target_compile_options(
    xnn_transfer_transfer
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_transfer
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_transfer)
endif()

if(XNN_TRANSFER_BUILD_BENCHMARKS)
  add_executable(
    xnn_transfer_transfer_benchmark
    transfer_benchmark.cpp
  )
  target_link_libraries(
    xnn_transfer_transfer_benchmark
    PRIVATE
      xnn_transfer_transfer
  )
  if(MSVC)
    target_compile_options(
      xnn_transfer_transfer_benchmark
      PRIVATE
        /W4
        /permissive-
    )
  else()
    target_compile_options(
      xnn_transfer_transfer_benchmark
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
  endif()

  add_custom_target(
    xnn_transfer_transfer_benchmark_run
    COMMAND "$<TARGET_FILE:xnn_transfer_transfer_benchmark>"
    DEPENDS xnn_transfer_transfer_benchmark
    USES_TERMINAL
    VERBATIM
  )
  cmake_language(
    DEFER
    DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    CALL add_dependencies
      xnn_transfer_engine_benchmark
      xnn_transfer_transfer_benchmark_run
  )
endif()
