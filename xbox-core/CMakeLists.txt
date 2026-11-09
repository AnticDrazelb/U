# xbox-core: runs original Xbox programs (XBEs) on the host CPU in 32-bit compatibility mode, with a
# high-level kernel. Desktop (Linux x86-64) or PS4 (with toolchain/ps4-cemu.cmake); see README.md.
cmake_minimum_required(VERSION 3.20)
project(xbox_core C CXX ASM)
if (PS4)
    # Crema's PS4 setup: its runtime (heap, mmap and thread wraps, crash log), -march=btver2, eboot.bin
    include(${CMAKE_CURRENT_SOURCE_DIR}/../toolchain/ps4-love-style.cmake)
endif()
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION XB_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(CORE_SOURCES src/memory.cpp src/xbe.cpp src/cpu.cpp src/switch.S src/kernel.cpp src/kernel_rtl.cpp src/kernel_crypto.cpp src/files.cpp src/xiso.cpp src/devices.cpp src/nv2a.cpp src/render.cpp src/draw.cpp src/pixel.cpp src/usb.cpp src/audio.cpp src/apu.cpp src/diagnostics.cpp src/gpu_vulkan.cpp)
# The GPU path's shaders: SPIR-V made at build time (nothing is compiled at run time, which the PS4
# couldn't do), as C arrays the code includes
find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
foreach(stage vert frag)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/shaders/nv2a_${stage}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
        COMMAND ${GLSLANG_VALIDATOR} -V --vn nv2a_${stage} -o ${CMAKE_BINARY_DIR}/shaders/nv2a_${stage}.h
                ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/nv2a.${stage}
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/nv2a.${stage})
    list(APPEND SHADER_HEADERS ${CMAKE_BINARY_DIR}/shaders/nv2a_${stage}.h)
endforeach()
set_source_files_properties(src/gpu_vulkan.cpp PROPERTIES OBJECT_DEPENDS "${SHADER_HEADERS}")
set(FRONTEND_SOURCES frontend/canvas.cpp frontend/library.cpp frontend/tools.cpp frontend/ui.cpp)
if (PS4)
    add_executable(xbox-core ${CORE_SOURCES} ${FRONTEND_SOURCES} ps4/platform_ps4.cpp ps4/main_ps4.cpp)
    # Crema's runtime, its log folder moved to /data/xbox-core (crash.log, boot-trace.log)
    set(RUNTIME_SOURCES)
    foreach(source ${PS4_RUNTIME_SOURCES})
        get_filename_component(name ${source} NAME)
        file(READ ${source} text)
        string(REPLACE "/data/CemuPS4" "/data/xbox-core/no-such-folder" text "${text}")
        string(REPLACE "/data/Crema" "/data/xbox-core" text "${text}")
        file(WRITE ${CMAKE_BINARY_DIR}/runtime/${name}.tmp "${text}")
        configure_file(${CMAKE_BINARY_DIR}/runtime/${name}.tmp ${CMAKE_BINARY_DIR}/runtime/${name} COPYONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
        list(APPEND RUNTIME_SOURCES ${CMAKE_BINARY_DIR}/runtime/${name})
    endforeach()
    # switch-bench's stubs, without the two crash-log hooks the app fills itself (the guest's state
    # in crash.log: ps4/main_ps4.cpp)
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/../switch-bench/src/ps4_stubs.cpp stubs)
    string(REPLACE "void ps4_crash_guest_info() {}" "" stubs "${stubs}")
    string(REPLACE "void ps4_crash_guest_regs(int) {}" "" stubs "${stubs}")
    file(WRITE ${CMAKE_BINARY_DIR}/runtime/ps4_stubs.cpp.tmp "${stubs}")
    configure_file(${CMAKE_BINARY_DIR}/runtime/ps4_stubs.cpp.tmp ${CMAKE_BINARY_DIR}/runtime/ps4_stubs.cpp COPYONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../switch-bench/src/ps4_stubs.cpp)
    target_sources(xbox-core PRIVATE ${RUNTIME_SOURCES} ${CMAKE_BINARY_DIR}/runtime/ps4_stubs.cpp)
    target_include_directories(xbox-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../port)
    target_link_libraries(xbox-core PRIVATE SceSysmodule SceSystemService SceUserService SceVideoOut SceAudioOut ScePad SceNet
        ${PS4_SYSROOT}/lib/libsmb2.a ${PS4_SYSROOT}/lib/libnfs.a)
    target_link_options(xbox-core PRIVATE --undefined=gethostbyname)
    # XB_IMAGE_BASE: link the app at this address instead of 0 (the system loads it at 0x400000,
    # inside the Xbox's addresses: games whose image reaches there won't load). As xbox-bench 01.04:
    # a copy of the link script that starts there; __dso_handle provided at its start, since lld's
    # (the ELF header's address 0) is out of reach of code linked above 2 GiB.
    set(XB_IMAGE_BASE "" CACHE STRING "link address of the app (empty: 0, as every PS4 app here)")
    if (XB_IMAGE_BASE)
        file(READ ${PS4_LINK_SCRIPT} xb_script)
        string(REGEX REPLACE "SECTIONS[ \t\r\n]*{" "SECTIONS\n{\n\t. = ${XB_IMAGE_BASE};\n\tPROVIDE_HIDDEN(__dso_handle = .);" xb_script "${xb_script}")
        file(WRITE ${CMAKE_BINARY_DIR}/xb-link.x "${xb_script}")
        string(REPLACE "${PS4_LINK_SCRIPT}" "${CMAKE_BINARY_DIR}/xb-link.x" CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
    endif()
    # the GPU path: RADV (scripts/build-mesa.sh) linked in, its headers from Mesa's tree. It makes the
    # app ~40 MiB, which at 0x400000 would take most of the Xbox's 64 MiB: only in builds linked away
    # from the Xbox's addresses (XB_IMAGE_BASE), unless asked for.
    if (XB_IMAGE_BASE)
        set(XB_GPU_LINK_DEFAULT ON)
    else()
        set(XB_GPU_LINK_DEFAULT OFF)
    endif()
    option(XB_GPU_LINK "link RADV: drawing on the PS4's GPU" ${XB_GPU_LINK_DEFAULT})
    set(PS4_MESA_SOURCE "$ENV{HOME}/cemups4-build/src/mesa" CACHE PATH "Mesa PS4 source directory")
    target_include_directories(xbox-core PRIVATE ${PS4_MESA_SOURCE}/include)
    # the package's variant, for its own files (the fault trail): where it is linked, and with RADV or not
    if (XB_IMAGE_BASE)
        set(XB_VARIANT_PLACE "1g")
    else()
        set(XB_VARIANT_PLACE "low")
    endif()
    if (XB_GPU_LINK)
        target_compile_definitions(xbox-core PRIVATE XB_VARIANT="${XB_VARIANT_PLACE}-gpu")
    else()
        target_compile_definitions(xbox-core PRIVATE XB_VARIANT="${XB_VARIANT_PLACE}-cpu")
    endif()
    if (XB_GPU_LINK)
        ps4_link_mesa(xbox-core)
        target_link_libraries(xbox-core PRIVATE ${PS4_SYSROOT}/lib/libz.a) # Mesa's
    else()
        target_compile_definitions(xbox-core PRIVATE XB_NO_VULKAN)
    endif()
    ps4_add_eboot(xbox-core)
else()
    add_executable(xbox-core ${CORE_SOURCES} src/platform_linux.cpp src/main.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(xbox-core PRIVATE Threads::Threads)
    # the PS4 front end's screen, drawn to PNG files
    add_executable(ui-preview ${FRONTEND_SOURCES} tools/ui-preview.cpp)
endif()
target_include_directories(xbox-core PRIVATE src ${CMAKE_BINARY_DIR}/shaders)
if (NOT PS4)
    target_link_libraries(xbox-core PRIVATE ${CMAKE_DL_LIBS}) # libvulkan, opened at run time
endif()
target_compile_definitions(xbox-core PRIVATE XB_VERSION="${XB_VERSION}")
set_source_files_properties(${CORE_SOURCES} src/platform_linux.cpp src/main.cpp ps4/platform_ps4.cpp ps4/main_ps4.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")
