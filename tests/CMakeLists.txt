add_executable(netpulse_tests
    test_cli.cpp
    test_icmp.cpp
    test_ip_address.cpp
    test_main.cpp
    test_report.cpp
    test_statistics.cpp)
target_include_directories(netpulse_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_compile_definitions(netpulse_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_link_libraries(netpulse_tests PRIVATE netpulse::core netpulse_warnings)
if(MINGW)
    target_link_options(netpulse_tests PRIVATE -static)
endif()

add_test(NAME unit_tests COMMAND netpulse_tests)

add_test(NAME cli_help COMMAND netpulse --help)
set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: netpulse")

add_test(NAME cli_version COMMAND netpulse --version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "^netpulse ${PROJECT_VERSION}")

add_test(NAME cli_requires_host COMMAND netpulse)
set_tests_properties(cli_requires_host PROPERTIES WILL_FAIL TRUE)

add_test(NAME cli_rejects_unknown_option COMMAND netpulse --bogus 127.0.0.1)
set_tests_properties(cli_rejects_unknown_option PROPERTIES WILL_FAIL TRUE)

# Sends real packets to the loopback interface. On Linux this needs root,
# CAP_NET_RAW or unprivileged ICMP sockets; without them the test is skipped.
add_test(NAME loopback_ping COMMAND netpulse -c 2 -i 0.2 127.0.0.1)
set_tests_properties(loopback_ping PROPERTIES
    PASS_REGULAR_EXPRESSION "2 packets transmitted, 2 received"
    SKIP_REGULAR_EXPRESSION "permission denied"
    TIMEOUT 30)
