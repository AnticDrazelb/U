# The Eden PS4 launcher, built inside Eden's tree with EDEN_EXTRA_FRONTEND_DIR (see ../build.sh).
set(EDEN_PS4_REPO ${CMAKE_CURRENT_LIST_DIR}/../..)

if (PS4)
    # Crema's PS4 setup: its runtime (heap, mmap and thread wraps, crash log), the eboot.bin rules
    include(${EDEN_PS4_REPO}/toolchain/ps4-love-style.cmake)
    file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/../VERSION EDEN_PS4_VERSION LIMIT_COUNT 1)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../VERSION)

    # Crema's runtime, with its log folder moved: crash.log, boot-trace.log (which it rotates) and
    # samples.log go to /data/eden-ps4, not into Crema's /data/Crema; and the one-time
    # /data/CemuPS4 -> /data/Crema migration is pointed at a folder that never exists.
    set(EDEN_PS4_RUNTIME_SOURCES)
    foreach(source ${PS4_RUNTIME_SOURCES})
        get_filename_component(name ${source} NAME)
        file(READ ${source} text)
        string(REPLACE "/data/CemuPS4" "/data/eden-ps4/no-such-folder" text "${text}")
        string(REPLACE "/data/Crema" "/data/eden-ps4" text "${text}")
        file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/runtime/${name}.tmp "${text}")
        configure_file(${CMAKE_CURRENT_BINARY_DIR}/runtime/${name}.tmp ${CMAKE_CURRENT_BINARY_DIR}/runtime/${name} COPYONLY)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${source})
        list(APPEND EDEN_PS4_RUNTIME_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/runtime/${name})
    endforeach()
    # The app heap leaves room in the system pool, the only memory that runs code, for the
    # recompiler's code caches (4 x 32 MiB).
    set_source_files_properties(${EDEN_PS4_RUNTIME_SOURCES} PROPERTIES
        COMPILE_DEFINITIONS PS4_SYSTEM_HEAP_MAX_MB=384
        INCLUDE_DIRECTORIES ${EDEN_PS4_REPO}/port)

    # The HOME menu (ui/), with its fonts (../ui-fonts) included whole
    set(EDEN_PS4_UI_SOURCES ui/canvas.cpp ui/fonts.cpp ui/art.cpp ui/home_menu.cpp ui/nro.cpp)
    set(EDEN_PS4_UI_FONTS ${CMAKE_CURRENT_LIST_DIR}/../ui-fonts)
    set_source_files_properties(ui/fonts.cpp PROPERTIES
        COMPILE_DEFINITIONS EDEN_PS4_UI_FONTS="${EDEN_PS4_UI_FONTS}"
        OBJECT_DEPENDS "${EDEN_PS4_UI_FONTS}/Inter-Regular.otf;${EDEN_PS4_UI_FONTS}/Inter-SemiBold.otf")
    # (stb's single-file libraries, compiled in canvas.cpp, aren't warning-clean)
    set_source_files_properties(ui/canvas.cpp PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-function;-Wno-conversion;-Wno-sign-conversion;-Wno-implicit-fallthrough;-Wno-missing-field-initializers")

    add_executable(eden-ps4 main.cpp platform_ps4.cpp ps4_stubs.cpp usb_import.cpp ${EDEN_PS4_UI_SOURCES}
                            ${EDEN_PS4_RUNTIME_SOURCES})
    target_include_directories(eden-ps4 PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_compile_definitions(eden-ps4 PRIVATE EDEN_PS4_VERSION="${EDEN_PS4_VERSION}")
    target_link_libraries(eden-ps4 PRIVATE SceSysmodule SceSystemService SceUserService SceVideoOut
        SceAudioOut ScePad SceNet
        # the runtime's network file system wraps (port/ps4_netfs.cpp)
        ${PS4_SYSROOT}/lib/libsmb2.a ${PS4_SYSROOT}/lib/libnfs.a)
    # the runtime's __real_gethostbyname: LLD 12's --wrap doesn't fetch libc.a's gethostbyname for
    # it alone, and create-fself can't import it
    target_link_options(eden-ps4 PRIVATE --undefined=gethostbyname)
    # Mesa's RADV, when scripts/build-mesa.sh has built it: Eden's Vulkan renderer on the PS4's GPU
    if (EXISTS ${PS4_MESA_BUILD}/src/amd/vulkan/libvulkan_radeon.a)
        ps4_link_mesa(eden-ps4)
        target_compile_definitions(eden-ps4 PRIVATE EDEN_PS4_RADV)
        message(STATUS "eden-ps4: Mesa's RADV linked: Eden's Vulkan renderer on the PS4's GPU")
    else()
        message(STATUS "eden-ps4: no Mesa build at ${PS4_MESA_BUILD}: the null renderer only")
    endif()
    ps4_add_eboot(eden-ps4)
else()
    add_executable(eden-ps4 main.cpp platform_host.cpp)
endif()

target_link_libraries(eden-ps4 PRIVATE common core audio_core video_core hid_core input_common
    frontend_common network Threads::Threads)
# vma.h (the Vulkan memory allocator, compiled in main.cpp) isn't warning-clean.
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS
    "-Wno-conversion;-Wno-unused-variable;-Wno-unused-parameter;-Wno-missing-field-initializers")
