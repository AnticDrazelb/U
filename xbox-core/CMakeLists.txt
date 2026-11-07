# xbox-core: runs original Xbox programs (XBEs) on the host CPU in 32-bit compatibility mode, with a
# high-level kernel. Desktop (Linux x86-64) build for now; see README.md.
cmake_minimum_required(VERSION 3.20)
project(xbox_core C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_executable(xbox-core src/main.cpp src/memory.cpp src/xbe.cpp src/cpu.cpp src/switch.S src/kernel.cpp
    src/files.cpp src/devices.cpp)
target_include_directories(xbox-core PRIVATE src)
target_compile_options(xbox-core PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(xbox-core PRIVATE Threads::Threads)
