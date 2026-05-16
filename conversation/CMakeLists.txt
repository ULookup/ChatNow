cmake_minimum_required(VERSION 3.1.3)
project(conversation_server)

set(target "conversation_server")

# 1. proto
set(proto_path ${CMAKE_CURRENT_SOURCE_DIR}/../proto)
set(proto_files common/types.proto common/error.proto common/envelope.proto
                message/message_types.proto
                message/message_service.proto
                identity/identity_service.proto
                conversation/conversation_service.proto)
set(proto_srcs "")
foreach(proto_file ${proto_files})
    string(REPLACE ".proto" ".pb.cc" proto_cc ${proto_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}${proto_cc})
        add_custom_command(
            PRE_BUILD
            COMMAND protoc
            ARGS --cpp_out=${CMAKE_CURRENT_BINARY_DIR} -I ${proto_path}
                 --experimental_allow_proto3_optional ${proto_path}/${proto_file}
            DEPENDS ${proto_path}/${proto_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
            COMMENT "生成Protobuf框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc}
        )
    endif()
    list(APPEND proto_srcs ${CMAKE_CURRENT_BINARY_DIR}/${proto_cc})
endforeach()

# 2. ODB
set(odb_path ${CMAKE_CURRENT_SOURCE_DIR}/../odb)
set(odb_files conversation_member.hxx conversation.hxx conversation_view.hxx)
set(odb_srcs "")
foreach(odb_file ${odb_files})
    string(REPLACE ".hxx" "-odb.cxx" odb_cxx ${odb_file})
    if(NOT EXISTS ${CMAKE_CURRENT_BINARY_DIR}${odb_cxx})
        add_custom_command(
            PRE_BUILD
            COMMAND odb
            ARGS -d mysql --std c++11 --generate-query --generate-schema
                 --profile boost/date-time ${odb_path}/${odb_file}
            DEPENDS ${odb_path}/${odb_file}
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}
            COMMENT "生成ODB框架代码文件:" ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx}
        )
    endif()
    list(APPEND odb_srcs ${CMAKE_CURRENT_BINARY_DIR}/${odb_cxx})
endforeach()

# 3. 源码
set(src_files "")
aux_source_directory(${CMAKE_CURRENT_SOURCE_DIR}/source src_files)

add_executable(${target} ${src_files} ${proto_srcs} ${odb_srcs})

target_link_libraries(${target} -lgflags -lspdlog
    -lfmt -lbrpc -lssl -lcrypto
    -lprotobuf -lleveldb -letcd-cpp-api
    -lcpprest -lcurl -lodb-mysql -lodb -lodb-boost
    -lcpr -lelasticlient -ljsoncpp
    -lhiredis -lredis++)

include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../common)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../odb)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../third/include)

INSTALL(TARGETS ${target} RUNTIME DESTINATION bin)
