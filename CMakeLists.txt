cmake_minimum_required(VERSION 3.24)

include(cmake/XnnVcpkg.cmake)

project(
  XnnTransfer
  VERSION 0.1.0
  DESCRIPTION "Peer-to-peer LAN file transfer"
  LANGUAGES CXX
)

option(XNN_TRANSFER_BUILD_TESTS "Build native tests" ON)
option(XNN_TRANSFER_BUILD_BENCHMARKS "Build native benchmarks" OFF)
option(XNN_TRANSFER_BUILD_FUZZERS "Build libFuzzer targets" OFF)
option(
  XNN_TRANSFER_ENABLE_ASAN_UBSAN
  "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
  OFF
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(XNN_TRANSFER_BUILD_TESTS)
  enable_testing()
endif()

include(cmake/XnnDependencies.cmake)

add_subdirectory(native)
