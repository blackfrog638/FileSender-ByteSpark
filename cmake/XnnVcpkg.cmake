include_guard(GLOBAL)

get_filename_component(
  _xnn_repository_root
  "${CMAKE_CURRENT_LIST_DIR}/.."
  ABSOLUTE
)

set(_xnn_vcpkg_commit "17f35ad2418007a895ced8a4cece4ab34068a58d")
if(DEFINED XNN_TRANSFER_VCPKG_ROOT)
  set(_xnn_vcpkg_root "${XNN_TRANSFER_VCPKG_ROOT}")
elseif(DEFINED ENV{XNN_TRANSFER_VCPKG_ROOT} AND
       NOT "$ENV{XNN_TRANSFER_VCPKG_ROOT}" STREQUAL "")
  set(_xnn_vcpkg_root "$ENV{XNN_TRANSFER_VCPKG_ROOT}")
elseif(DEFINED ENV{VCPKG_ROOT} AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
  set(_xnn_vcpkg_root "$ENV{VCPKG_ROOT}")
else()
  set(_xnn_vcpkg_root "${_xnn_repository_root}/out/tools/vcpkg")
endif()
get_filename_component(_xnn_vcpkg_root "${_xnn_vcpkg_root}" ABSOLUTE)

set(_xnn_vcpkg_toolchain
    "${_xnn_vcpkg_root}/scripts/buildsystems/vcpkg.cmake")
if(NOT EXISTS "${_xnn_vcpkg_toolchain}")
  message(
    FATAL_ERROR
    "Pinned vcpkg is missing at ${_xnn_vcpkg_root}. "
    "Run `make vcpkg-bootstrap` from the repository root."
  )
endif()

execute_process(
  COMMAND git -C "${_xnn_vcpkg_root}" rev-parse HEAD
  RESULT_VARIABLE _xnn_vcpkg_git_result
  OUTPUT_VARIABLE _xnn_vcpkg_actual_commit
  ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _xnn_vcpkg_git_result EQUAL 0 OR
   NOT _xnn_vcpkg_actual_commit STREQUAL _xnn_vcpkg_commit)
  message(
    FATAL_ERROR
    "vcpkg checkout must be exactly ${_xnn_vcpkg_commit}; "
    "found '${_xnn_vcpkg_actual_commit}' at ${_xnn_vcpkg_root}."
  )
endif()

if(DEFINED CMAKE_TOOLCHAIN_FILE AND
   NOT "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "" AND
   NOT "${CMAKE_TOOLCHAIN_FILE}" STREQUAL "${_xnn_vcpkg_toolchain}")
  message(
    FATAL_ERROR
    "XnnTransfer requires the pinned vcpkg toolchain; "
    "received ${CMAKE_TOOLCHAIN_FILE}."
  )
endif()

set(
  CMAKE_TOOLCHAIN_FILE
  "${_xnn_vcpkg_toolchain}"
  CACHE FILEPATH
  "Pinned XnnTransfer vcpkg toolchain"
  FORCE
)
set(
  VCPKG_MANIFEST_DIR
  "${_xnn_repository_root}"
  CACHE PATH
  "XnnTransfer vcpkg manifest directory"
  FORCE
)
set(
  VCPKG_INSTALLED_DIR
  "${_xnn_repository_root}/out/vcpkg_installed"
  CACHE PATH
  "Ignored XnnTransfer vcpkg installation"
  FORCE
)
set(
  VCPKG_OVERLAY_TRIPLETS
  "${_xnn_repository_root}/cmake/triplets"
  CACHE STRING
  "XnnTransfer static dependency triplets"
  FORCE
)

set(_xnn_host_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
if("${_xnn_host_processor}" STREQUAL "")
  cmake_host_system_information(
    RESULT _xnn_host_processor
    QUERY OS_PLATFORM
  )
endif()
string(TOLOWER "${_xnn_host_processor}" _xnn_host_processor)

if(NOT DEFINED VCPKG_TARGET_TRIPLET OR
   "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
  if(CMAKE_HOST_WIN32)
    set(_xnn_triplet "xnn-x64-windows-static")
  elseif(CMAKE_HOST_APPLE)
    set(_xnn_architectures "${CMAKE_OSX_ARCHITECTURES}")
    if("${_xnn_architectures}" STREQUAL "")
      set(_xnn_architectures "${_xnn_host_processor}")
    endif()
    string(REPLACE " " ";" _xnn_architectures "${_xnn_architectures}")
    list(REMOVE_DUPLICATES _xnn_architectures)
    list(LENGTH _xnn_architectures _xnn_architecture_count)
    if(_xnn_architecture_count GREATER 1)
      set(_xnn_triplet "xnn-universal-osx-static")
    elseif(_xnn_architectures MATCHES "^(arm64|aarch64)$")
      set(_xnn_triplet "xnn-arm64-osx-static")
    elseif(_xnn_architectures MATCHES "^(x86_64|amd64|x64)$")
      set(_xnn_triplet "xnn-x64-osx-static")
    else()
      message(
        FATAL_ERROR
        "Unsupported macOS architecture '${_xnn_architectures}'."
      )
    endif()
  elseif(CMAKE_HOST_UNIX)
    if(_xnn_host_processor MATCHES "^(aarch64|arm64)$")
      set(_xnn_triplet "xnn-arm64-linux-static")
    elseif(_xnn_host_processor MATCHES "^(x86_64|amd64|x64)$")
      set(_xnn_triplet "xnn-x64-linux-static")
    else()
      message(
        FATAL_ERROR
        "Unsupported Linux architecture '${_xnn_host_processor}'."
      )
    endif()
  else()
    message(FATAL_ERROR "Unsupported dependency host platform.")
  endif()
  set(
    VCPKG_TARGET_TRIPLET
    "${_xnn_triplet}"
    CACHE STRING
    "XnnTransfer static dependency triplet"
    FORCE
  )
endif()

if(NOT EXISTS
   "${_xnn_repository_root}/cmake/triplets/${VCPKG_TARGET_TRIPLET}.cmake")
  message(
    FATAL_ERROR
    "Unsupported XnnTransfer vcpkg triplet '${VCPKG_TARGET_TRIPLET}'."
  )
endif()
