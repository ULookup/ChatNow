# 1. 添加 cmake 版本说明
cmake_minimum_required(VERSION 3.1.3)
# 2. 声明工程名称
project(message_server)
# 3. 添加子目录
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/message)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/user)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/file)
# speech 子服务在 P4 已并入 media_server（位于 file/）；speech/ 目录已删除。
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/transmite)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/relationship)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/chatsession)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/conversation)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/gateway)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/push)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/common/test)
# 4.
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR})