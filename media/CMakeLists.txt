# 1. cmake 版本
cmake_minimum_required(VERSION 3.1.3)
# 2. 工程名
project(media_server)

set(target "media_server")
set(test_client "media_client")

# 3.1 protoc：生成 proto 框架代码
set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(proto_files
    common/types.proto
    common/error.proto
    common/envelope.proto
    media/media_service.proto)
set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    string(REPLACE ".proto" ".pb.h"  proto_hh ${proto_file})
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
        COMMAND protoc
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             -I ${proto_path}
             --experimental_allow_proto3_optional
             ${proto_path}/${proto_file}
        DEPENDS ${proto_path}/${proto_file}
        COMMENT "生成 Protobuf 框架: ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}")
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

# 3.2 ODB：生成数据库映射代码 + DDL（--generate-schema）
set(odb_path ${CMAKE_CURRENT_SOURCE_DIR}/../odb)
set(odb_files
    media_file.hxx
    media_blob_ref.hxx
    media_user_quota.hxx)
set(odb_srcs "")
foreach(odb_file ${odb_files})
    string(REPLACE ".hxx" "-odb.hxx" odb_hxx ${odb_file})
    string(REPLACE ".hxx" "-odb.cxx" odb_cxx ${odb_file})
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}
        COMMAND odb
        ARGS -d mysql --std c++11
             --generate-query
             --generate-schema
             --profile boost/date-time
             ${odb_path}/${odb_file}
        DEPENDS ${odb_path}/${odb_file}
        COMMENT "生成 ODB 框架: ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}")
    list(APPEND odb_srcs ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx})
endforeach()

# 4. 主程序源码
set(src_files
    ${CMAKE_CURRENT_SOURCE_DIR}/source/media_main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/source/media_server.h
    ${CMAKE_CURRENT_SOURCE_DIR}/source/upload_handler.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/multipart_handler.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/download_handler.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/speech_handler.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/source/cleanup_worker.hpp)
add_executable(${target} ${src_files} ${proto_srcs} ${odb_srcs})

target_link_libraries(${target}
    -lgflags -lspdlog -lfmt
    -lbrpc -lssl -lcrypto -lprotobuf -lleveldb -letcd-cpp-api
    -lcpprest -lcurl
    -lodb-mysql -lodb -lodb-boost
    -lredis++ -lhiredis
    -laws-cpp-sdk-s3 -laws-cpp-sdk-core
    /usr/local/lib/libjsoncpp.so.19)

# 5. 测试可执行（gtest 单元 + 集成）
set(test_files
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_media_dao_integration.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/test/test_s3_integration.cc)
# FIXME(3.0): media_client test has gflags linking issues with brpc static lib
# add_executable(${test_client} ${test_files} ${proto_srcs} ${odb_srcs})
# target_link_libraries(${test_client}
#     -lgflags -lgtest -lgtest_main -lspdlog -lfmt
#     -lbrpc -lssl -lcrypto -lprotobuf -lleveldb -letcd-cpp-api
#     -lcpprest -lcurl
#     -lodb-mysql -lodb -lodb-boost
#     -lredis++ -lhiredis
#     -laws-cpp-sdk-s3 -laws-cpp-sdk-core
#     /usr/local/lib/libjsoncpp.so.19)

# 6. 头文件搜索路径
target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../odb)
target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

# target_include_directories(${test_client} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
# target_include_directories(${test_client} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
# target_include_directories(${test_client} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
# target_include_directories(${test_client} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../odb)
# target_include_directories(${test_client} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

INSTALL(TARGETS ${target} RUNTIME DESTINATION bin)
