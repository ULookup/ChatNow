# 1. 添加 cmake 版本说明
cmake_minimum_required(VERSION 3.1.3)
# 2. 声明工程名称
project(message_server)
# 3. 添加子目录
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/message)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/user)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/file)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/speech)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/transmite)
# 4.
set(CMAKE_INSTALL_PREFIX ${CMAKE_CURRENT_BINARY_DIR})