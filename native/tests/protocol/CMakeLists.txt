add_executable(
  xnn_transfer_protocol_tests
  v1_parser_test.cpp
)
target_link_libraries(xnn_transfer_protocol_tests PRIVATE xnn_transfer_protocol)

if(XNN_TRANSFER_ENABLE_ASAN_UBSAN)
  xnn_transfer_enable_sanitizers(xnn_transfer_protocol_tests)
endif()

add_test(NAME xnn_transfer_protocol_tests COMMAND xnn_transfer_protocol_tests)
