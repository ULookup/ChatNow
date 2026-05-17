cmake_minimum_required(VERSION 3.1.3)
project(presence_server)

set(target "presence_server")

set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(proto_files
    common/types.proto
    common/error.proto
    common/envelope.proto
    presence/presence_service.proto
    push/push_service.proto
    push/notify.proto
)

set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    string(REPLACE ".proto" ".pb.h" proto_hh ${proto_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
        add_custom_command(
            PRE_BUILD
            COMMAND protoc
            ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I ${proto_path}
                 --experimental_allow_proto3_optional ${proto_path}/${proto_file}
            DEPENDS ${proto_path}/${proto_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
            COMMENT "生成Protobuf框架代码: ${proto_cc}"
        )
    endif()
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

set(src_files "")
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/source src_files)

add_executable(${target} ${src_files} ${proto_srcs})

target_link_libraries(${target} -lgflags -lspdlog
    -lfmt -lbrpc -lssl -lcrypto
    -lprotobuf -lleveldb -letcd-cpp-api
    -lcurl -lhiredis -lredis++ -lpthread -lboost_system)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

INSTALL(TARGETS ${target} RUNTIME DESTINATION bin)
