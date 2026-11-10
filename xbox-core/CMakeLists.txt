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

# this build's identity (cmake/build-id.cmake), written anew each build
add_custom_target(build_id ALL
    COMMAND ${CMAKE_COMMAND} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/build_id.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build-id.cmake
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/build_id.cpp)
# xemu's APU (GPL-2.0-or-later) and libsamplerate (BSD-2-Clause): src/third_party
set(XEMU_DIR src/third_party/xemu/hw/xbox/mcpx/apu)
set(XEMU_APU_SOURCES ${XEMU_DIR}/apu.c ${XEMU_DIR}/vp/vp.c ${XEMU_DIR}/monitor.c ${XEMU_DIR}/debug.c ${XEMU_DIR}/dsp/gp_ep.c
    ${XEMU_DIR}/dsp/dsp.c ${XEMU_DIR}/dsp/dsp_c.c ${XEMU_DIR}/dsp/dsp_dma.c ${XEMU_DIR}/dsp/interp/dsp_cpu.c
    src/third_party/libsamplerate/samplerate.c src/third_party/libsamplerate/src_sinc.c
    src/third_party/libsamplerate/src_linear.c src/third_party/libsamplerate/src_zoh.c
    src/third_party/xemu/hw/xbox/mcpx/nvnet/nvnet.c src/third_party/xemu/shim/xb_qemu_timer.c
    src/third_party/xemu/shim/xb_bql.c
    src/third_party/xemu/hw/usb/core.c src/third_party/xemu/hw/usb/desc.c src/third_party/xemu/hw/usb/hcd-ohci.c
    src/third_party/xemu/hw/usb/dev-hub.c src/third_party/xemu/hw/usb/desc-msos.c src/third_party/xemu/hw/usb/xb_bus.c src/third_party/xemu/hw/usb/xb_usb.c
    src/third_party/xemu/hw/xbox/xid.c src/third_party/xemu/hw/xbox/xid-gamepad.c)
set(CORE_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/build_id.cpp src/memory.cpp src/xbe.cpp src/cpu.cpp src/switch.S src/kernel.cpp src/kernel_rtl.cpp src/kernel_crypto.cpp src/files.cpp src/xiso.cpp src/devices.cpp src/nv2a.cpp src/render.cpp src/draw.cpp src/pixel.cpp src/usb.cpp src/audio.cpp src/apu.cpp src/xemu_glue.cpp ${XEMU_APU_SOURCES} src/nic.cpp src/diagnostics.cpp src/gpu_vulkan.cpp)
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
# the fragment shader that writes depth itself (w-buffer, polygon offset): only for draws that need it,
# as writing depth turns off the GPU's early depth test
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/shaders/nv2a_frag_depth.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLANG_VALIDATOR} -V -DXB_DEPTH --vn nv2a_frag_depth -o ${CMAKE_BINARY_DIR}/shaders/nv2a_frag_depth.h
            ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/nv2a.frag
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/nv2a.frag)
list(APPEND SHADER_HEADERS ${CMAKE_BINARY_DIR}/shaders/nv2a_frag_depth.h)
set_source_files_properties(src/gpu_vulkan.cpp PROPERTIES OBJECT_DEPENDS "${SHADER_HEADERS}")
set(FRONTEND_SOURCES frontend/canvas.cpp frontend/library.cpp frontend/tools.cpp frontend/ui.cpp)
if (PS4)
    add_executable(xbox-core ${CORE_SOURCES} ${FRONTEND_SOURCES} ps4/platform_ps4.cpp ps4/main_ps4.cpp)
    add_dependencies(xbox-core build_id)
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
    add_dependencies(xbox-core build_id)
    find_package(Threads REQUIRED)
    target_link_libraries(xbox-core PRIVATE Threads::Threads)
    # the PS4 front end's screen, drawn to PNG files
    add_executable(ui-preview ${FRONTEND_SOURCES} tools/ui-preview.cpp)
endif()
target_include_directories(xbox-core PRIVATE src ${CMAKE_BINARY_DIR}/shaders)
# xemu's code: its QEMU headers stood in for by shim/
set_source_files_properties(${XEMU_APU_SOURCES} PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/xemu/shim;${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/xemu;${CMAKE_CURRENT_SOURCE_DIR}/${XEMU_DIR}/dsp;${CMAKE_CURRENT_SOURCE_DIR}/${XEMU_DIR}/dsp/interp;${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/libsamplerate;${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/xemu/hw/xbox/mcpx/nvnet;${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/xemu/hw/usb"
    COMPILE_DEFINITIONS "HAVE_STDBOOL_H;PACKAGE=\"libsamplerate\";VERSION=\"0.2.2\";ENABLE_SINC_FAST_CONVERTER;CPU_CLIPS_POSITIVE=0;CPU_CLIPS_NEGATIVE=0"
    COMPILE_OPTIONS "-w")
set_source_files_properties(src/apu.cpp src/xemu_glue.cpp src/usb.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/xemu/shim;${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/xemu")
if (NOT PS4)
    target_link_libraries(xbox-core PRIVATE ${CMAKE_DL_LIBS}) # libvulkan, opened at run time
endif()

# xemu's NV2A (GPL-2.0-or-later) with its Vulkan renderer, where xbox-core has Vulkan: its
# dependencies at xemu's revisions (glslang compiles its generated shaders at run time; volk loads
# Vulkan; VMA, SPIRV-Reflect, xxHash, nv2a_vsh_cpu). src/nv2a.cpp chooses it at start (XB_NV2A=own:
# xbox-core's own).
if (NOT PS4 OR XB_GPU_LINK)
    set(TP ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party)
    set(XEMU_NV2A_DIR ${TP}/xemu/hw/xbox/nv2a)
    file(GLOB XEMU_NV2A_SOURCES ${XEMU_NV2A_DIR}/*.c ${XEMU_NV2A_DIR}/pgraph/*.c ${XEMU_NV2A_DIR}/pgraph/glsl/*.c
         ${XEMU_NV2A_DIR}/pgraph/vk/*.c)
    list(FILTER XEMU_NV2A_SOURCES EXCLUDE REGEX "debug_renderdoc\\.c$")
    list(APPEND XEMU_NV2A_SOURCES ${TP}/xemu/shim/xb_vga.c ${TP}/xemu/shim/xb_memory.c ${TP}/xemu/shim/fast-hash.c
         ${TP}/nv2a_vsh_cpu/nv2a_vsh_cpu.c ${TP}/nv2a_vsh_cpu/nv2a_vsh_disassembler.c ${TP}/nv2a_vsh_cpu/nv2a_vsh_emulator.c
         ${TP}/nv2a_vsh_cpu/nv2a_vsh_emulator_execution_state.c ${TP}/volk/volk.c ${TP}/spirv-reflect/spirv_reflect.c
         ${TP}/vma/vma_impl.cpp)
    target_sources(xbox-core PRIVATE ${XEMU_NV2A_SOURCES})
    set(XEMU_NV2A_INCLUDES "${TP}/xemu/shim;${TP}/xemu;${XEMU_NV2A_DIR};${TP}/xxhash;${TP}/volk;${TP}/vma;${TP}/spirv-reflect;${TP}/spirv-reflect/include;${TP}/glslang;${TP}/nv2a_vsh_cpu")
    if (PS4)
        set(XEMU_NV2A_INCLUDES "${XEMU_NV2A_INCLUDES};${PS4_MESA_SOURCE}/include")
        set(XEMU_NV2A_DEFS "VK_NO_PROTOTYPES;XB_VK_ICD_LINKED")
    else()
        set(XEMU_NV2A_DEFS "VK_NO_PROTOTYPES")
    endif()
    set_source_files_properties(${XEMU_NV2A_SOURCES} PROPERTIES INCLUDE_DIRECTORIES "${XEMU_NV2A_INCLUDES}"
        COMPILE_DEFINITIONS "${XEMU_NV2A_DEFS}" COMPILE_OPTIONS "-w")
    set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
    set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
    set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
    set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
    set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
    add_subdirectory(${TP}/glslang ${CMAKE_BINARY_DIR}/glslang EXCLUDE_FROM_ALL)
    target_link_libraries(xbox-core PRIVATE glslang glslang-default-resource-limits)
    target_compile_definitions(xbox-core PRIVATE XB_XEMU_NV2A)
endif()
target_compile_definitions(xbox-core PRIVATE XB_VERSION="${XB_VERSION}")
set_source_files_properties(${CORE_SOURCES} src/platform_linux.cpp src/main.cpp ps4/platform_ps4.cpp ps4/main_ps4.cpp
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")
