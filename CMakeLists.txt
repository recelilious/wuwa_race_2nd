cmake_minimum_required(VERSION 3.24)

project(wuwa_race_2nd LANGUAGES CXX)

option(WUWA_ENABLE_CUDA "Build CUDA simulation backend" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(wuwa_compile_options INTERFACE)
if(MSVC)
    target_compile_options(wuwa_compile_options INTERFACE "$<$<COMPILE_LANGUAGE:CXX>:/utf-8>")
endif()

add_library(wuwa_sim
    src/scenario.cpp
    src/scenario_io.cpp
    src/sim_cpu.cpp
)

target_include_directories(wuwa_sim PUBLIC include)
target_link_libraries(wuwa_sim PUBLIC wuwa_compile_options)

add_executable(wuwa_race_sim src/main.cpp)
target_link_libraries(wuwa_race_sim PRIVATE wuwa_sim)

add_executable(wuwa_trace_decode src/trace_decode.cpp)
target_link_libraries(wuwa_trace_decode PRIVATE wuwa_compile_options)

if(WUWA_ENABLE_CUDA)
    enable_language(CUDA)
    add_library(wuwa_sim_cuda src/sim_cuda.cu)
    target_include_directories(wuwa_sim_cuda PUBLIC include)
    set_target_properties(wuwa_sim_cuda PROPERTIES
        CUDA_STANDARD 20
        CUDA_STANDARD_REQUIRED ON
        CUDA_ARCHITECTURES 89
    )
    target_compile_definitions(wuwa_race_sim PRIVATE WUWA_ENABLE_CUDA=1)
    target_link_libraries(wuwa_race_sim PRIVATE wuwa_sim_cuda)
endif()
