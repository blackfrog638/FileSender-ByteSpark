add_library(
  xnn_transfer_core
  SHARED
  src/bridge/c_api.cpp
  src/core/engine.cpp
)

find_package(Threads REQUIRED)

function(xnn_transfer_enable_sanitizers target)
  if(MSVC)
    message(FATAL_ERROR "ASan/UBSan preset currently requires Clang or GCC")
  endif()

  target_compile_options(
    ${target}
    PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
  )
  target_link_options(
    ${target}
    PRIVATE
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
  )
endfunction()

add_subdirectory(src/protocol)

target_include_directories(
  xnn_transfer_core
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
)
target_link_libraries(xnn_transfer_core PRIVATE Threads::Threads)

target_compile_definitions(xnn_transfer_core PRIVATE XNN_TRANSFER_BUILDING_LIBRARY)
set_target_properties(
  xnn_transfer_core
  PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME xnn_transfer_core
)

if(MSVC)
  target_compile_options(xnn_transfer_core PRIVATE /W4 /permissive-)
else()
  target_compile_options(
    xnn_transfer_core
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN OR XNN_TRANSFER_BUILD_FUZZERS)
  xnn_transfer_enable_sanitizers(xnn_transfer_core)
endif()

if(XNN_TRANSFER_BUILD_TESTS)
  add_subdirectory(tests/protocol)

  add_executable(
    xnn_transfer_native_tests
    tests/native_tests.cpp
  )
  target_link_libraries(xnn_transfer_native_tests PRIVATE xnn_transfer_core)
  if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
    xnn_transfer_enable_sanitizers(xnn_transfer_native_tests)
  endif()

  add_test(NAME xnn_transfer_native_tests COMMAND xnn_transfer_native_tests)

  add_executable(
    xnn_transfer_bridge_event_tests
    tests/bridge/c_api_event_test.cpp
  )
  target_link_libraries(
    xnn_transfer_bridge_event_tests
    PRIVATE
      Threads::Threads
      xnn_transfer_core
  )
  if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
    xnn_transfer_enable_sanitizers(xnn_transfer_bridge_event_tests)
  endif()

  add_test(
    NAME xnn_transfer_bridge_event_tests
    COMMAND xnn_transfer_bridge_event_tests
  )
endif()

if(XNN_TRANSFER_BUILD_BENCHMARKS)
  add_executable(
    xnn_transfer_engine_benchmark
    benchmarks/engine_lifecycle_benchmark.cpp
  )
  target_link_libraries(xnn_transfer_engine_benchmark PRIVATE xnn_transfer_core)
endif()

if(XNN_TRANSFER_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "XNN_TRANSFER_BUILD_FUZZERS requires Clang")
  endif()

  add_subdirectory(fuzz/protocol)

  add_executable(
    xnn_transfer_c_api_fuzzer
    fuzz/c_api_fuzzer.cpp
  )
  target_link_libraries(xnn_transfer_c_api_fuzzer PRIVATE xnn_transfer_core)
  target_compile_options(xnn_transfer_c_api_fuzzer PRIVATE -fsanitize=fuzzer)
  target_link_options(xnn_transfer_c_api_fuzzer PRIVATE -fsanitize=fuzzer)
  xnn_transfer_enable_sanitizers(xnn_transfer_c_api_fuzzer)
endif()
