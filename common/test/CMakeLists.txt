# common 公共组件单元测试
cmake_minimum_required(VERSION 3.1.3)
project(common_tests)

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

# 一个可执行文件聚合所有 test_*.cc 源文件
set(common_test_target common_tests)
file(GLOB test_srcs ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cc)

add_executable(${common_test_target} ${test_srcs} ${proto_srcs})

target_link_libraries(${common_test_target}
    -lgflags -lgtest -lgtest_main
    -lspdlog -lfmt
    -lbrpc -lssl -lcrypto -lprotobuf -lleveldb
    -lcpprest -lcurl
    /usr/local/lib/libjsoncpp.so.19
)

INSTALL(TARGETS ${common_test_target} RUNTIME DESTINATION bin)
