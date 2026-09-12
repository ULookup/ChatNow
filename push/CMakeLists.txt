cmake_minimum_required(VERSION 3.12)
project(push_server)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(target "push_server")
# 1. proto
set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(proto_files common/types.proto common/error.proto common/envelope.proto common/auth/metadata.proto message/message_types.proto message/message_service.proto message/message_internal.proto push/push_service.proto push/notify.proto presence/presence_service.proto)
set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    string(REPLACE ".proto" ".pb.h" proto_h ${proto_file})
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
               ${CMAKE_CURRENT_BINARY_DIR}/${proto_h}
        COMMAND protoc
        ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
             -I ${proto_path}
             --experimental_allow_proto3_optional
             ${proto_path}/${proto_file}
        DEPENDS ${proto_path}/${proto_file}
        COMMENT "Generating protobuf: ${proto_file}"
    )
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

# 2. 源码
set(src_files "")
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/source src_files)
add_executable(${target} ${src_files} ${proto_srcs})

find_package(jsoncpp CONFIG REQUIRED)
target_link_libraries(${target} jsoncpp_lib -lgflags -lspdlog -lglog
    -lfmt -lbrpc -lssl -lcrypto
    -lprotobuf -lleveldb -letcd-cpp-api
    -lcpprest -lcurl -lamqpcpp -lev
    -lhiredis -lredis++
    -lpthread -lboost_system)

target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

INSTALL(TARGETS ${target} RUNTIME DESTINATION bin)
