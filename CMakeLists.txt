cmake_minimum_required(VERSION 3.9)
project(json_parser)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native -Wall -Wextra -Wpedantic -Wno-implicit-fallthrough -Wno-missing-field-initializers -Wno-unused-parameter")

set(SOURCE_FILES
        json.c)

add_library(${PROJECT_NAME} ${SOURCE_FILES})
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

