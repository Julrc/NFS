cmake_minimum_required(VERSION 3.16)

project(file_system CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Debug)
endif()

add_executable(file_system
src/main.cc
src/fs_commands.cc
src/fs_core.cc
)

target_include_directories(file_system PRIVATE
include)

target_compile_options(file_system PRIVATE
-Wall -Wextra -Wpedantic)

