include_guard(GLOBAL)

if(NOT DEFINED VCPKG_TARGET_TRIPLET OR
   NOT VCPKG_TARGET_TRIPLET MATCHES "^xnn-.*-static$")
  message(
    FATAL_ERROR
    "P1 dependencies require a project-owned static vcpkg triplet."
  )
endif()

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(asio CONFIG REQUIRED)
find_package(OpenSSL 3.5.7 EXACT REQUIRED)
find_package(utf8proc 2.11.3 EXACT CONFIG REQUIRED)

set(XNN_TRANSFER_HAS_LIBSECRET FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(PkgConfig REQUIRED)

  set(_xnn_had_pkg_config_libdir FALSE)
  if(DEFINED ENV{PKG_CONFIG_LIBDIR})
    set(_xnn_had_pkg_config_libdir TRUE)
    set(_xnn_saved_pkg_config_libdir "$ENV{PKG_CONFIG_LIBDIR}")
  endif()
  set(_xnn_had_pkg_config_path FALSE)
  if(DEFINED ENV{PKG_CONFIG_PATH})
    set(_xnn_had_pkg_config_path TRUE)
    set(_xnn_saved_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
  endif()

  set(
    _xnn_pkgconfig_root
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}"
  )
  set(
    _xnn_pkgconfig_dirs
    "${_xnn_pkgconfig_root}/lib/pkgconfig"
    "${_xnn_pkgconfig_root}/share/pkgconfig"
  )
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(
      _xnn_pkgconfig_dirs
      "${_xnn_pkgconfig_root}/debug/lib/pkgconfig"
      ${_xnn_pkgconfig_dirs}
    )
  endif()
  list(JOIN _xnn_pkgconfig_dirs ":" _xnn_pkg_config_libdir)
  set(ENV{PKG_CONFIG_LIBDIR} "${_xnn_pkg_config_libdir}")
  set(ENV{PKG_CONFIG_PATH} "")
  pkg_check_modules(
    Libsecret
    REQUIRED
    NO_CMAKE_PATH
    NO_CMAKE_ENVIRONMENT_PATH
    libsecret-1=0.21.7
  )

  if(_xnn_had_pkg_config_libdir)
    set(ENV{PKG_CONFIG_LIBDIR} "${_xnn_saved_pkg_config_libdir}")
  else()
    unset(ENV{PKG_CONFIG_LIBDIR})
  endif()
  if(_xnn_had_pkg_config_path)
    set(ENV{PKG_CONFIG_PATH} "${_xnn_saved_pkg_config_path}")
  else()
    unset(ENV{PKG_CONFIG_PATH})
  endif()

  get_filename_component(
    _xnn_pkgconfig_root_real
    "${_xnn_pkgconfig_root}"
    REALPATH
  )
  function(xnn_require_vcpkg_dependency_path dependency_path label)
    if(NOT IS_ABSOLUTE "${dependency_path}")
      message(FATAL_ERROR "${label} is not absolute: ${dependency_path}")
    endif()
    if(NOT EXISTS "${dependency_path}")
      message(FATAL_ERROR "${label} does not exist: ${dependency_path}")
    endif()
    get_filename_component(
      _xnn_dependency_path_real
      "${dependency_path}"
      REALPATH
    )
    file(
      RELATIVE_PATH
      _xnn_dependency_relative_path
      "${_xnn_pkgconfig_root_real}"
      "${_xnn_dependency_path_real}"
    )
    if(_xnn_dependency_relative_path STREQUAL ".." OR
       _xnn_dependency_relative_path MATCHES "^\\.\\./" OR
       IS_ABSOLUTE "${_xnn_dependency_relative_path}")
      message(
        FATAL_ERROR
        "${label} escaped the pinned vcpkg root: ${dependency_path}"
      )
    endif()
  endfunction()

  foreach(
    _xnn_include_directory
    IN LISTS
      Libsecret_STATIC_INCLUDE_DIRS
  )
    xnn_require_vcpkg_dependency_path(
      "${_xnn_include_directory}"
      "libsecret include directory"
    )
  endforeach()
  foreach(
    _xnn_library_directory
    IN LISTS
      Libsecret_STATIC_LIBRARY_DIRS
  )
    xnn_require_vcpkg_dependency_path(
      "${_xnn_library_directory}"
      "libsecret library directory"
    )
  endforeach()

  set(_xnn_system_link_libraries dl m pthread rt -pthread)
  set(_xnn_libsecret_link_libraries)
  foreach(_xnn_link_library IN LISTS Libsecret_STATIC_LIBRARIES)
    if(IS_ABSOLUTE "${_xnn_link_library}")
      xnn_require_vcpkg_dependency_path(
        "${_xnn_link_library}"
        "libsecret link library"
      )
      if(NOT _xnn_link_library MATCHES "\\.a$")
        message(
          FATAL_ERROR
          "libsecret resolved a non-static library: ${_xnn_link_library}"
        )
      endif()
      list(APPEND _xnn_libsecret_link_libraries "${_xnn_link_library}")
    elseif(_xnn_link_library IN_LIST _xnn_system_link_libraries)
      list(APPEND _xnn_libsecret_link_libraries "${_xnn_link_library}")
    else()
      unset(_xnn_resolved_library)
      unset(_xnn_resolved_library CACHE)
      find_library(
        _xnn_resolved_library
        NAMES "${_xnn_link_library}"
        PATHS ${Libsecret_STATIC_LIBRARY_DIRS}
        NO_DEFAULT_PATH
      )
      if(NOT _xnn_resolved_library)
        message(
          FATAL_ERROR
          "libsecret dependency is absent from pinned vcpkg: "
          "${_xnn_link_library}"
        )
      endif()
      xnn_require_vcpkg_dependency_path(
        "${_xnn_resolved_library}"
        "libsecret link library"
      )
      if(NOT _xnn_resolved_library MATCHES "\\.a$")
        message(
          FATAL_ERROR
          "libsecret resolved a non-static library: ${_xnn_resolved_library}"
        )
      endif()
      list(APPEND _xnn_libsecret_link_libraries "${_xnn_resolved_library}")
    endif()
  endforeach()

  add_library(xnn_libsecret INTERFACE)
  target_include_directories(
    xnn_libsecret
    SYSTEM
    INTERFACE
      ${Libsecret_STATIC_INCLUDE_DIRS}
  )
  target_link_directories(
    xnn_libsecret
    INTERFACE
      ${Libsecret_STATIC_LIBRARY_DIRS}
  )
  target_link_libraries(
    xnn_libsecret
    INTERFACE
      ${_xnn_libsecret_link_libraries}
  )
  target_compile_options(
    xnn_libsecret
    INTERFACE
      ${Libsecret_STATIC_CFLAGS_OTHER}
  )
  target_link_options(
    xnn_libsecret
    INTERFACE
      ${Libsecret_STATIC_LDFLAGS_OTHER}
  )
  add_library(XnnDependencies::libsecret ALIAS xnn_libsecret)
  set(XNN_TRANSFER_HAS_LIBSECRET TRUE)
endif()

foreach(
  dependency_target
  IN ITEMS
    asio::asio
    OpenSSL::SSL
    OpenSSL::Crypto
    utf8proc::utf8proc
)
  if(NOT TARGET "${dependency_target}")
    message(FATAL_ERROR "Missing pinned dependency target ${dependency_target}")
  endif()
endforeach()

if(NOT OPENSSL_VERSION STREQUAL "3.5.7")
  message(FATAL_ERROR "Expected OpenSSL 3.5.7, found ${OPENSSL_VERSION}")
endif()
if(DEFINED utf8proc_VERSION AND NOT utf8proc_VERSION STREQUAL "2.11.3")
  message(FATAL_ERROR "Expected utf8proc 2.11.3, found ${utf8proc_VERSION}")
endif()

foreach(
  static_target
  IN ITEMS
    OpenSSL::SSL
    OpenSSL::Crypto
    utf8proc::utf8proc
)
  get_target_property(_xnn_dependency_type "${static_target}" TYPE)
  if(_xnn_dependency_type STREQUAL "SHARED_LIBRARY" OR
     _xnn_dependency_type STREQUAL "MODULE_LIBRARY")
    message(
      FATAL_ERROR
      "${static_target} resolved to dynamic type ${_xnn_dependency_type}"
    )
  endif()
endforeach()

message(
  STATUS
  "Pinned P1 dependencies: Asio 1.38.2, OpenSSL ${OPENSSL_VERSION}, "
  "utf8proc 2.11.3"
)
if(XNN_TRANSFER_HAS_LIBSECRET)
  message(STATUS "Pinned Linux dependency: libsecret ${Libsecret_VERSION}")
endif()
message(STATUS "Pinned vcpkg triplet: ${VCPKG_TARGET_TRIPLET}")

if(XNN_TRANSFER_BUILD_TESTS)
  add_executable(
    xnn_transfer_dependency_probe
    "${CMAKE_CURRENT_LIST_DIR}/dependencies/dependency_probe.cpp"
  )
  target_compile_features(xnn_transfer_dependency_probe PRIVATE cxx_std_20)
  target_compile_definitions(
    xnn_transfer_dependency_probe
    PRIVATE
      ASIO_NO_DEPRECATED
      ASIO_STANDALONE
  )
  target_link_libraries(
    xnn_transfer_dependency_probe
    PRIVATE
      asio::asio
      OpenSSL::SSL
      OpenSSL::Crypto
      utf8proc::utf8proc
  )
  if(XNN_TRANSFER_HAS_LIBSECRET)
    target_compile_definitions(
      xnn_transfer_dependency_probe
      PRIVATE
        XNN_TRANSFER_HAS_LIBSECRET=1
    )
    target_link_libraries(
      xnn_transfer_dependency_probe
      PRIVATE
        XnnDependencies::libsecret
    )
  endif()
  add_test(
    NAME xnn_transfer_dependency_probe
    COMMAND xnn_transfer_dependency_probe
  )
  set_tests_properties(xnn_transfer_dependency_probe PROPERTIES TIMEOUT 30)
  if(XNN_TRANSFER_HAS_LIBSECRET)
    set_tests_properties(
      xnn_transfer_dependency_probe
      PROPERTIES
        PASS_REGULAR_EXPRESSION "libsecret 0\\.21\\.7"
    )
  endif()
endif()
