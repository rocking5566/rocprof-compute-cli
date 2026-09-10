add_executable(rcv_core_link_test rcv_core_link_test.cpp)
target_include_directories(rcv_core_link_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(rcv_core_link_test GTest::gtest_main rcv_core)
add_test(NAME rcv_core_link_test COMMAND rcv_core_link_test)

add_executable(trace_loader_test trace_loader_test.cpp)
target_include_directories(trace_loader_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(trace_loader_test GTest::gtest_main rcv_core)
add_test(NAME trace_loader_test COMMAND trace_loader_test)
set_tests_properties(trace_loader_test PROPERTIES ENVIRONMENT
    "RCV_CLI_TEST_TRACE=$ENV{RCV_CLI_TEST_TRACE}")

add_executable(digest_test digest_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest_builder.cpp)
target_include_directories(digest_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(digest_test GTest::gtest_main rcv_core)
add_test(NAME digest_test COMMAND digest_test)
set_tests_properties(digest_test PROPERTIES ENVIRONMENT
    "RCV_CLI_TEST_TRACE=$ENV{RCV_CLI_TEST_TRACE};RCV_CLI_TEST_STATS_CSV=$ENV{RCV_CLI_TEST_STATS_CSV}")

add_executable(hidden_latency_test hidden_latency_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest_builder.cpp)
target_include_directories(hidden_latency_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(hidden_latency_test GTest::gtest_main rcv_core)
add_test(NAME hidden_latency_test COMMAND hidden_latency_test)
set_tests_properties(hidden_latency_test PROPERTIES ENVIRONMENT
    "RCV_CLI_TEST_TRACE=$ENV{RCV_CLI_TEST_TRACE}")

add_executable(occupancy_test occupancy_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest_builder.cpp)
target_include_directories(occupancy_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(occupancy_test GTest::gtest_main rcv_core)
add_test(NAME occupancy_test COMMAND occupancy_test)

add_executable(queries_test queries_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/queries.cpp)
target_include_directories(queries_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(queries_test GTest::gtest_main rcv_core)
add_test(NAME queries_test COMMAND queries_test)

add_executable(format_test format_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/digest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/queries.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/cli/format.cpp)
target_include_directories(format_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
target_link_libraries(format_test GTest::gtest_main rcv_core)
add_test(NAME format_test COMMAND format_test)
