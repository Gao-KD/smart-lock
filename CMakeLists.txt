cmake_minimum_required(VERSION 3.16)
project(smart_lock C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Host build: shared protocol library + lock firmware logic with a host HAL,
# so protocol and lock logic are unit-tested without hardware.
# The EFR32 build uses Simplicity Studio / SLC, see firmware/lock/port/efr32.
option(SMARTLOCK_BUILD_TESTS "Build unit tests" ON)

if(SMARTLOCK_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(common/slp)
add_subdirectory(firmware)
