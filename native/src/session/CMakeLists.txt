add_library(
  xnn_transfer_session
  STATIC
  admission.cpp
  authority.cpp
  channels.cpp
  connection_runtime.cpp
  established_connection.cpp
  pairing.cpp
  pairing_socket.cpp
  tls_handshake.cpp
  wire.cpp
)

target_compile_features(xnn_transfer_session PUBLIC cxx_std_20)
set_target_properties(
  xnn_transfer_session
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(
  xnn_transfer_session
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>"
    "$<INSTALL_INTERFACE:include>"
)

target_link_libraries(
  xnn_transfer_session
  PUBLIC
    xnn_transfer_identity
    xnn_transfer_tls
  PRIVATE
    asio::asio
    OpenSSL::Crypto
    OpenSSL::SSL
)

if(MSVC)
  target_compile_options(
    xnn_transfer_session
    PRIVATE
      /W4
      /permissive-
  )
else()
  target_compile_options(
    xnn_transfer_session
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
  )
endif()

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_session)
endif()
