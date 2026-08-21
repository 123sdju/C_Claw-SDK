cmake_minimum_required(VERSION 3.16)
project(cclaw_consumer_test LANGUAGES C)

find_package(CClaw CONFIG REQUIRED)

add_executable(cclaw_consumer main.c)
target_link_libraries(cclaw_consumer PRIVATE CClaw::runtime)
