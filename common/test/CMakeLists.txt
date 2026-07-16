# common 公共组件单元测试
cmake_minimum_required(VERSION 3.1.3)
project(common_tests)

include(CheckIncludeFileCXX)

# proto 编译（仅 error.proto，单元测试需要 ErrorCode enum）
set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../../proto)
set(proto_files common/error.proto)
set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
        add_custom_command(
            PRE_BUILD
            COMMAND protoc
            ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I ${proto_path} --experimental_allow_proto3_optional ${proto_path}/${proto_file}
            DEPENDS ${proto_path}/${proto_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
            COMMENT "生成Protobuf框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
        )
    endif()
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../../third/include)

add_executable(test_reliability_state test_reliability_state.cc)
set_property(TARGET test_reliability_state PROPERTY CXX_STANDARD 17)
set_property(TARGET test_reliability_state PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_reliability_state RUNTIME DESTINATION bin)

add_executable(test_local_cache test_local_cache.cc)
set_property(TARGET test_local_cache PROPERTY CXX_STANDARD 17)
set_property(TARGET test_local_cache PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_local_cache RUNTIME DESTINATION bin)

add_executable(test_local_cache_metrics test_local_cache_metrics.cc)
set_property(TARGET test_local_cache_metrics PROPERTY CXX_STANDARD 17)
set_property(TARGET test_local_cache_metrics PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_local_cache_metrics RUNTIME DESTINATION bin)

add_executable(test_local_cache_callbacks test_local_cache_callbacks.cc)
set_property(TARGET test_local_cache_callbacks PROPERTY CXX_STANDARD 17)
set_property(TARGET test_local_cache_callbacks PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_local_cache_callbacks RUNTIME DESTINATION bin)

add_executable(test_local_cache_callback_exceptions test_local_cache_callback_exceptions.cc)
set_property(TARGET test_local_cache_callback_exceptions PROPERTY CXX_STANDARD 17)
set_property(TARGET test_local_cache_callback_exceptions PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_local_cache_callback_exceptions RUNTIME DESTINATION bin)

add_executable(test_local_cache_ttl test_local_cache_ttl.cc)
set_property(TARGET test_local_cache_ttl PROPERTY CXX_STANDARD 17)
set_property(TARGET test_local_cache_ttl PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_local_cache_ttl RUNTIME DESTINATION bin)

add_executable(test_local_cache_expiry_priority test_local_cache_expiry_priority.cc)
set_property(TARGET test_local_cache_expiry_priority PROPERTY CXX_STANDARD 17)
set_property(TARGET test_local_cache_expiry_priority PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_local_cache_expiry_priority RUNTIME DESTINATION bin)

add_executable(test_local_cache_zero_capacity test_local_cache_zero_capacity.cc)
set_property(TARGET test_local_cache_zero_capacity PROPERTY CXX_STANDARD 17)
set_property(TARGET test_local_cache_zero_capacity PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_local_cache_zero_capacity RUNTIME DESTINATION bin)

add_executable(test_inflight test_inflight.cc)
set_property(TARGET test_inflight PROPERTY CXX_STANDARD 17)
set_property(TARGET test_inflight PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_inflight RUNTIME DESTINATION bin)

add_executable(test_seq_keys test_seq_keys.cc)
set_property(TARGET test_seq_keys PROPERTY CXX_STANDARD 17)
set_property(TARGET test_seq_keys PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_seq_keys RUNTIME DESTINATION bin)

add_executable(test_members_cache_rules test_members_cache_rules.cc)
set_property(TARGET test_members_cache_rules PROPERTY CXX_STANDARD 17)
set_property(TARGET test_members_cache_rules PROPERTY CXX_STANDARD_REQUIRED ON)
INSTALL(TARGETS test_members_cache_rules RUNTIME DESTINATION bin)

check_include_file_cxx("sw/redis++/redis++.h" CHATNOW_HAS_REDIS_PLUS_PLUS)
if(CHATNOW_HAS_REDIS_PLUS_PLUS)
    add_executable(test_redis_mutex_contract test_redis_mutex_contract.cc)
    set_property(TARGET test_redis_mutex_contract PROPERTY CXX_STANDARD 17)
    set_property(TARGET test_redis_mutex_contract PROPERTY CXX_STANDARD_REQUIRED ON)
    INSTALL(TARGETS test_redis_mutex_contract RUNTIME DESTINATION bin)

    add_executable(test_unacked_pending_ledger test_unacked_pending_ledger.cc)
    set_property(TARGET test_unacked_pending_ledger PROPERTY CXX_STANDARD 17)
    set_property(TARGET test_unacked_pending_ledger PROPERTY CXX_STANDARD_REQUIRED ON)
    target_link_libraries(test_unacked_pending_ledger
        -lredis++ -lhiredis -lbrpc -lspdlog -lfmt -lpthread)
    INSTALL(TARGETS test_unacked_pending_ledger RUNTIME DESTINATION bin)
endif()

# FIXME(3.0): common_tests has gflags link order issue with brpc static lib
# set(common_test_target common_tests)
# file(GLOB test_srcs ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc)
# add_executable(${common_test_target} ${test_srcs} ${proto_srcs})
# target_link_libraries(${common_test_target}
#     -lgflags -lgtest -lgtest_main
#     -lspdlog -lfmt
#     -lbrpc -lssl -lcrypto -lprotobuf -lleveldb
#     -lcpprest -lcurl
#     /usr/local/lib/libjsoncpp.so.19
# )
# INSTALL(TARGETS ${common_test_target} RUNTIME DESTINATION bin)
