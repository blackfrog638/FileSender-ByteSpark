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
  "utf8proc 2.11.3 (${VCPKG_TARGET_TRIPLET})"
)

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
  add_test(
    NAME xnn_transfer_dependency_probe
    COMMAND xnn_transfer_dependency_probe
  )
  set_tests_properties(xnn_transfer_dependency_probe PROPERTIES TIMEOUT 30)
endif()
