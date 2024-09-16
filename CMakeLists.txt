cmake_minimum_required(VERSION 3.10)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)

project(TrafficExample)

add_executable(TrafficExample 
    ${CMAKE_SOURCE_DIR}/examples/amba_tlm/TrafficExample.cpp
    ${CMAKE_SOURCE_DIR}/examples/amba_tlm/Memory.cpp
    ${CMAKE_SOURCE_DIR}/examples/amba_tlm/Monitor.cpp
    ${CMAKE_SOURCE_DIR}/examples/amba_tlm/TrafficGenerator.cpp
    ${CMAKE_SOURCE_DIR}/src/libarmaxi4.cpp
)

find_library(SYSTEMC_LIB systemc PATHS /00_ycl/04_install/systemc-2.3.4/lib-linux64)
target_link_libraries(TrafficExample PUBLIC ${SYSTEMC_LIB})  

target_include_directories(TrafficExample PUBLIC
                          "${CMAKE_SOURCE_DIR}/examples/amba_tlm/"
                          "${CMAKE_SOURCE_DIR}/include/"
                          "/00_ycl/04_install/systemc-2.3.4/include"
                          )
                          
target_compile_options(TrafficExample PRIVATE "-g")                          