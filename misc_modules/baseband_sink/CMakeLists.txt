cmake_minimum_required(VERSION 3.13)
project(baseband_sink)

file(GLOB SRC "src/*.cpp" "../../baseband_common/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(baseband_sink PRIVATE "src/" "../../baseband_common")