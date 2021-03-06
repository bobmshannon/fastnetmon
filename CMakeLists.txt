cmake_minimum_required (VERSION 2.8)

# cmake versions:
# Debian 6 - 2.8.2
# Debian 7 - 2.8.9
# CentOS 6 - 2.8.12

project(FastNetMon)

set (Tutorial_VERSION_MAJOR 1)
set (Tutorial_VERSION_MINOR 1)

# It's pretty safe and provide big speedup for our packet processor and patricia code
set(CMAKE_C_FLAGS "-O2")
set(CMAKE_CXX_FLAGS "-O2")

set(FASTNETMON_PROFILER OFF)

set(FASTNETMON_PROFILE_FLAGS "-g -pg")

if (FASTNETMON_PROFILER) 
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FASTNETMON_PROFILE_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FASTNETMON_PROFILE_FLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS  "${CMAKE_EXE_LINKER_FLAGS} ${FASTNETMON_PROFILE_FLAGS}")
endif()

# Set path to home compiled PF_RING
set(PFRING_INCLUDE_DIRS /opt/pf_ring/include)
set(PFRING_LIBRARIES /opt/pf_ring/lib/libpfring.so)

# Some defines for FastNetMon compiation tuning
add_definitions(-DPF_RING)

# If you need hardware locking features
# add_definitions(-DHWFILTER_LOCKING)

# Our LPM library
add_library(patricia STATIC libpatricia/patricia.c)

# Our sFLOW plugin
add_library(sflow_plugin STATIC sflow_plugin/sflow_collector.cpp) 

# Main tool
add_executable(fastnetmon fastnetmon.cpp)

# Client tool
add_executable(fastnetmon_client fastnetmon_client.cpp)

# Find boost: http://www.cmake.org/cmake/help/v3.0/module/FindBoost.html

# Disable cmake script from Boost package becaus it is broken: http://public.kitware.com/Bug/view.php?id=15270
set(Boost_NO_BOOST_CMAKE ON)
# Enable detailed errors
set(Boost_DETAILED_FAILURE_MSG ON)

find_package(Boost COMPONENTS thread regex REQUIRED)

if(Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
    target_link_libraries(fastnetmon ${Boost_LIBRARIES})
endif()

# Try to find ncurses library
find_package(Curses REQUIRED)

if(CURSES_FOUND) 
    include_directories(${CURSES_INCLUDE_DIRS})
    target_link_libraries(fastnetmon_client ${CURSES_LIBRARIES})
endif()

# External libs
target_link_libraries(fastnetmon log4cpp)
target_link_libraries(fastnetmon numa)
target_link_libraries(fastnetmon pthread)
# For PF_RING
target_link_libraries(fastnetmon pcap)

# Our libs
target_link_libraries(fastnetmon patricia)

# Our plugins
target_link_libraries(fastnetmon sflow_plugin)

# Custom PF_RING
include_directories(${PFRING_INCLUDE_DIRS})
target_link_libraries(fastnetmon ${PFRING_LIBRARIES})
