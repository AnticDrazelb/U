# Switch-CPU benchmark: Eden's dynarmic (AArch64 -> x86-64) running AArch64 kernels, for the host
# (a quick check) or the PS4 (with toolchain/ps4-cemu.cmake; see README.md).
cmake_minimum_required(VERSION 3.20)
project(switch_bench C CXX)
if (PS4)
    # Crema's PS4 setup: its runtime (heap, mmap and thread wraps), -march=btver2, eboot.bin rules
    include(${CMAKE_CURRENT_SOURCE_DIR}/../toolchain/ps4-love-style.cmake)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(EDEN_DIR "$ENV{HOME}/eden" CACHE PATH "Eden checkout (patches/dynarmic-ps4-code-memory.patch applied)")
set(XBYAK_DIR "$ENV{HOME}/sbdeps/xbyak" CACHE PATH "xbyak checkout (v7.40.1)")
set(HEADERS_DIR "$ENV{HOME}/cemups4-build/sysroot/include" CACHE PATH "where boost/ and fmt/ headers are")
set(GUEST_CC clang-21 CACHE STRING "clang that targets aarch64")
set(GUEST_LD ld.lld-21 CACHE STRING "lld for the guest image")

set(DYNARMIC_DIR ${EDEN_DIR}/src/dynarmic/src/dynarmic)
if (NOT EXISTS ${DYNARMIC_DIR}/backend/x64/a64_interface.cpp)
    message(FATAL_ERROR "dynarmic not found under EDEN_DIR=${EDEN_DIR}")
endif()

# Only boost/ and fmt/ from HEADERS_DIR (it can be a sysroot with other libraries' headers too).
set(DEPS_INCLUDE ${CMAKE_BINARY_DIR}/deps-include)
file(MAKE_DIRECTORY ${DEPS_INCLUDE})
foreach(lib boost fmt)
    if (NOT EXISTS ${DEPS_INCLUDE}/${lib})
        file(CREATE_LINK ${HEADERS_DIR}/${lib} ${DEPS_INCLUDE}/${lib} SYMBOLIC)
    endif()
endforeach()

add_library(bench_deps INTERFACE)
target_include_directories(bench_deps SYSTEM INTERFACE ${DEPS_INCLUDE} ${XBYAK_DIR})
# fmt header-only, so the host and the PS4 use the same fmt without building it
target_compile_definitions(bench_deps INTERFACE FMT_HEADER_ONLY=1 FMT_USE_USER_DEFINED_LITERALS=1)

# dynarmic: the x86-64 backend's sources as Eden builds them (src/dynarmic/src/dynarmic/CMakeLists.txt)
file(GLOB DYNARMIC_SOURCES
    ${DYNARMIC_DIR}/backend/block_range_information.cpp
    ${DYNARMIC_DIR}/backend/exception_handler_generic.cpp
    ${DYNARMIC_DIR}/backend/x64/*.cpp
    ${DYNARMIC_DIR}/common/*.cpp
    ${DYNARMIC_DIR}/common/crypto/*.cpp
    ${DYNARMIC_DIR}/common/fp/*.cpp
    ${DYNARMIC_DIR}/common/fp/op/*.cpp
    ${DYNARMIC_DIR}/frontend/*.cpp
    ${DYNARMIC_DIR}/frontend/A32/*.cpp
    ${DYNARMIC_DIR}/frontend/A32/translate/*.cpp
    ${DYNARMIC_DIR}/frontend/A64/*.cpp
    ${DYNARMIC_DIR}/frontend/A64/translate/*.cpp
    ${DYNARMIC_DIR}/ir/*.cpp)
list(FILTER DYNARMIC_SOURCES EXCLUDE REGEX "(exception_handler_windows|spin_lock_(arm64|riscv64|loongarch64))\\.cpp$")
add_library(dynarmic STATIC ${DYNARMIC_SOURCES})
# DYNARMIC_DIR/.. for "dynarmic/...", Eden's src/ for "common/..." (its headers only; the few
# functions dynarmic needs from it are in shim/common_shim.cpp)
target_include_directories(dynarmic PUBLIC ${DYNARMIC_DIR}/.. ${EDEN_DIR}/src)
target_compile_definitions(dynarmic PUBLIC ARCHITECTURE_x86_64=1
    PRIVATE XBYAK_STRICT_CHECK_MEM_REG_SIZE=0 XBYAK_OLD_DISP_CHECK=1
            DYNARMIC_ENABLE_CPU_FEATURE_DETECTION=1 MCL_IGNORE_ASSERTS=1)
if (BENCH_HOST_RWX_ALLOCATOR)
    # the PS4's code allocator (__ORBIS__ selects it there), on the host to test it
    target_compile_definitions(dynarmic PRIVATE DYNARMIC_RWX_CODE_ALLOCATOR=1)
endif()
target_compile_options(dynarmic PRIVATE -Wno-unused-parameter -Wno-unused-command-line-argument -fbracket-depth=1024)
target_link_libraries(dynarmic PUBLIC bench_deps)

# The guest: guest/bench.c for AArch64 (a Cortex-A57, the Switch's CPU), linked at 0x80000000
set(GUEST_ELF ${CMAKE_BINARY_DIR}/bench_guest.elf)
set(GUEST_HEADER ${CMAKE_BINARY_DIR}/guest_image.h)
add_custom_command(OUTPUT ${GUEST_ELF}
    COMMAND ${GUEST_CC} --target=aarch64-none-elf -mcpu=cortex-a57 -O2 -ffreestanding -nostdlib
            -ffp-contract=off -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables
            -c ${CMAKE_SOURCE_DIR}/guest/bench.c -o ${CMAKE_BINARY_DIR}/bench_guest.o
    COMMAND ${GUEST_LD} -T ${CMAKE_SOURCE_DIR}/guest/link.ld -static
            ${CMAKE_BINARY_DIR}/bench_guest.o -o ${GUEST_ELF}
    DEPENDS guest/bench.c guest/link.ld
    COMMENT "Building the AArch64 guest kernels")
add_custom_command(OUTPUT ${GUEST_HEADER}
    COMMAND ${CMAKE_COMMAND} -DIN=${GUEST_ELF} -DOUT=${GUEST_HEADER} -DNAME=guest_image
            -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${GUEST_ELF} cmake/embed.cmake)

# The same kernels natively, for comparison
add_library(native_kernels OBJECT guest/bench.c)
target_compile_definitions(native_kernels PRIVATE BENCH_PREFIX=native_)
target_compile_options(native_kernels PRIVATE -O2 -ffp-contract=off)

if (PS4)
    set(PLATFORM_SOURCE src/platform_ps4.cpp)
else()
    set(PLATFORM_SOURCE src/platform_host.cpp)
endif()
add_executable(switch_bench src/bench_main.cpp ${PLATFORM_SOURCE} shim/common_shim.cpp ${GUEST_HEADER}
    $<TARGET_OBJECTS:native_kernels>)
target_include_directories(switch_bench PRIVATE src ${CMAKE_BINARY_DIR})
target_link_libraries(switch_bench PRIVATE dynarmic)
if (PS4)
    # Crema's runtime, with its log folder moved: crash.log, boot-trace.log (which it rotates) and
    # samples.log go to /data/switch-bench, not into Crema's /data/Crema; and the one-time
    # /data/CemuPS4 -> /data/Crema migration is pointed at a folder that never exists.
    set(BENCH_RUNTIME_SOURCES)
    foreach(source ${PS4_RUNTIME_SOURCES})
        get_filename_component(name ${source} NAME)
        file(READ ${source} text)
        string(REPLACE "/data/CemuPS4" "/data/switch-bench/no-such-folder" text "${text}")
        string(REPLACE "/data/Crema" "/data/switch-bench" text "${text}")
        file(WRITE ${CMAKE_BINARY_DIR}/runtime/${name}.tmp "${text}")
        configure_file(${CMAKE_BINARY_DIR}/runtime/${name}.tmp ${CMAKE_BINARY_DIR}/runtime/${name} COPYONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
        list(APPEND BENCH_RUNTIME_SOURCES ${CMAKE_BINARY_DIR}/runtime/${name})
    endforeach()
    target_sources(switch_bench PRIVATE ${BENCH_RUNTIME_SOURCES} src/ps4_stubs.cpp)
    target_include_directories(switch_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../port)
    target_link_libraries(switch_bench PRIVATE SceSysmodule SceSystemService SceUserService SceVideoOut SceNet
        # the runtime's network file system wraps (port/ps4_netfs.cpp)
        ${PS4_SYSROOT}/lib/libsmb2.a ${PS4_SYSROOT}/lib/libnfs.a)
    # the runtime's __real_gethostbyname: LLD 12's --wrap doesn't fetch libc.a's gethostbyname for
    # it alone (in Crema other objects pull that member in), and create-fself can't import it
    target_link_options(switch_bench PRIVATE --undefined=gethostbyname)
    ps4_add_eboot(switch_bench)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(switch_bench PRIVATE Threads::Threads m)
endif()
