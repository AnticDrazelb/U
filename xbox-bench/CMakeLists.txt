# Xbox CPU Bench: original Xbox (32-bit x86, Pentium III) kernels run natively in 32-bit
# compatibility mode, for the host (a quick check) or the PS4 (with toolchain/ps4-cemu.cmake; see
# README.md).
cmake_minimum_required(VERSION 3.20)
project(xbox_bench C CXX ASM)
if (PS4)
    # Crema's PS4 setup: its runtime (heap, mmap and thread wraps), -march=btver2, eboot.bin rules
    include(${CMAKE_CURRENT_SOURCE_DIR}/../toolchain/ps4-love-style.cmake)
endif()

file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION BENCH_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(HEADERS_DIR "$ENV{HOME}/cemups4-build/sysroot/include" CACHE PATH "where the fmt/ headers are")
set(GUEST_CC clang CACHE STRING "clang that targets i386")
set(GUEST_LD ld.lld CACHE STRING "lld for the guest image")

# Only fmt/ from HEADERS_DIR (it can be a sysroot with other libraries' headers too).
set(DEPS_INCLUDE ${CMAKE_BINARY_DIR}/deps-include)
file(MAKE_DIRECTORY ${DEPS_INCLUDE})
if (NOT EXISTS ${DEPS_INCLUDE}/fmt)
    file(CREATE_LINK ${HEADERS_DIR}/fmt ${DEPS_INCLUDE}/fmt SYMBOLIC)
endif()

# The guest: guest/kernels.c for a Pentium III (MMX, SSE; doubles on the x87), linked twice: at
# 0x00010000 (an XBE's base) and at 0x40010000 (the fallback layout).
set(GUEST_FLAGS --target=i386-none-elf -march=pentium3 -mfpmath=sse -O2 -ffreestanding -nostdlib
    -ffp-contract=off -fno-stack-protector -fno-pic -fno-asynchronous-unwind-tables)
set(GUEST_OBJ ${CMAKE_BINARY_DIR}/kernels_guest.o)
add_custom_command(OUTPUT ${GUEST_OBJ}
    COMMAND ${GUEST_CC} ${GUEST_FLAGS} -c ${CMAKE_SOURCE_DIR}/guest/kernels.c -o ${GUEST_OBJ}
    DEPENDS guest/kernels.c
    COMMENT "Building the Pentium III guest kernels")
set(GUEST_HEADERS)
foreach(variant "guest_image;0x00010000" "guest_image_high;0x40010000")
    list(GET variant 0 name)
    list(GET variant 1 base)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${name}.elf
        COMMAND ${GUEST_LD} -m elf_i386 -T ${CMAKE_SOURCE_DIR}/guest/link.ld --defsym=IMAGE_BASE=${base} -static
                ${GUEST_OBJ} -o ${CMAKE_BINARY_DIR}/${name}.elf
        DEPENDS ${GUEST_OBJ} guest/link.ld)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${name}.h
        COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_BINARY_DIR}/${name}.elf -DOUT=${CMAKE_BINARY_DIR}/${name}.h -DNAME=${name}
                -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS ${CMAKE_BINARY_DIR}/${name}.elf cmake/embed.cmake)
    list(APPEND GUEST_HEADERS ${CMAKE_BINARY_DIR}/${name}.h)
endforeach()

# The same kernels natively, for comparison
add_library(native_kernels OBJECT guest/kernels.c)
target_compile_definitions(native_kernels PRIVATE BENCH_PREFIX=native_)
target_compile_options(native_kernels PRIVATE -O2 -ffp-contract=off)

if (PS4)
    set(PLATFORM_SOURCE src/platform_ps4.cpp)
else()
    set(PLATFORM_SOURCE src/platform_host.cpp)
endif()
add_executable(xbox_bench src/bench_main.cpp src/trampoline.S ${PLATFORM_SOURCE} ${GUEST_HEADERS}
    $<TARGET_OBJECTS:native_kernels>)
target_include_directories(xbox_bench PRIVATE src ${CMAKE_BINARY_DIR})
target_include_directories(xbox_bench SYSTEM PRIVATE ${DEPS_INCLUDE})
target_compile_definitions(xbox_bench PRIVATE BENCH_VERSION="${BENCH_VERSION}" FMT_HEADER_ONLY=1)
if (PS4)
    # Crema's runtime, with its log folder moved: crash.log, boot-trace.log (which it rotates) and
    # samples.log go to /data/xbox-bench, not into Crema's /data/Crema; and the one-time
    # /data/CemuPS4 -> /data/Crema migration is pointed at a folder that never exists.
    set(BENCH_RUNTIME_SOURCES)
    foreach(source ${PS4_RUNTIME_SOURCES})
        get_filename_component(name ${source} NAME)
        file(READ ${source} text)
        string(REPLACE "/data/CemuPS4" "/data/xbox-bench/no-such-folder" text "${text}")
        string(REPLACE "/data/Crema" "/data/xbox-bench" text "${text}")
        file(WRITE ${CMAKE_BINARY_DIR}/runtime/${name}.tmp "${text}")
        configure_file(${CMAKE_BINARY_DIR}/runtime/${name}.tmp ${CMAKE_BINARY_DIR}/runtime/${name} COPYONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
        list(APPEND BENCH_RUNTIME_SOURCES ${CMAKE_BINARY_DIR}/runtime/${name})
    endforeach()
    # the emulator hooks Crema's crash log calls (switch-bench's stand-ins, mprotect included)
    target_sources(xbox_bench PRIVATE ${BENCH_RUNTIME_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/../switch-bench/src/ps4_stubs.cpp)
    target_include_directories(xbox_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../port)
    target_link_libraries(xbox_bench PRIVATE SceSysmodule SceSystemService SceUserService SceVideoOut ScePad SceNet
        # the runtime's network file system wraps (port/ps4_netfs.cpp)
        ${PS4_SYSROOT}/lib/libsmb2.a ${PS4_SYSROOT}/lib/libnfs.a)
    target_link_options(xbox_bench PRIVATE --undefined=gethostbyname)
    # XB_IMAGE_BASE: link the program at this address instead of 0 (where the system places it is
    # what the bench's memory map shows): the Xbox's own addresses below 0x980000 are taken by
    # the program when it loads at 0x400000. A copy of the link script that starts there.
    # __dso_handle (only an identity for __cxa_atexit) is lld's otherwise, at the ELF header's
    # address 0, out of reach of code linked above 2 GiB.
    set(XB_IMAGE_BASE "" CACHE STRING "link address of the program (empty: 0, as every PS4 app here)")
    if (XB_IMAGE_BASE)
        file(READ ${PS4_LINK_SCRIPT} xb_script)
        string(REGEX REPLACE "SECTIONS[ \t\r\n]*{" "SECTIONS\n{\n\t. = ${XB_IMAGE_BASE};\n\tPROVIDE_HIDDEN(__dso_handle = .);" xb_script "${xb_script}")
        file(WRITE ${CMAKE_BINARY_DIR}/xb-link.x "${xb_script}")
        string(REPLACE "${PS4_LINK_SCRIPT}" "${CMAKE_BINARY_DIR}/xb-link.x" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
        target_compile_definitions(xbox_bench PRIVATE XB_IMAGE_BASE_TEXT="${XB_IMAGE_BASE}")
    endif()
    ps4_add_eboot(xbox_bench)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(xbox_bench PRIVATE Threads::Threads m)
endif()
