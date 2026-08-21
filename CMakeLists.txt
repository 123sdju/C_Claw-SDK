cmake_minimum_required(VERSION 3.16)

project(CClawSDK VERSION 0.1.0 LANGUAGES C)

set(CCLAW_VERSION "${PROJECT_VERSION}" CACHE STRING "CClaw SDK version")
set(CC_PROFILE "core-minimal" CACHE STRING "SDK build profile")
set(CC_TARGET_PLATFORM "auto" CACHE STRING "SDK target platform")
set(CCLAW_PROFILE_FILE "" CACHE FILEPATH
    "Optional external CClaw profile file")

add_subdirectory(cclaw)
