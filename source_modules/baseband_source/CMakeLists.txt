cmake_minimum_required(VERSION 3.13)
project(baseband_source)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(baseband_source PRIVATE "src/")