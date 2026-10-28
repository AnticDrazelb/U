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

    add_executable(eden-ps4 main.cpp platform_ps4.cpp ps4_stubs.cpp ${EDEN_PS4_RUNTIME_SOURCES})
    target_compile_definitions(eden-ps4 PRIVATE EDEN_PS4_VERSION="${EDEN_PS4_VERSION}")
    target_link_libraries(eden-ps4 PRIVATE SceSysmodule SceSystemService SceUserService SceVideoOut
        SceAudioOut ScePad SceNet
        # the runtime's network file system wraps (port/ps4_netfs.cpp)
        ${PS4_SYSROOT}/lib/libsmb2.a ${PS4_SYSROOT}/lib/libnfs.a)
    # the runtime's __real_gethostbyname: LLD 12's --wrap doesn't fetch libc.a's gethostbyname for
    # it alone, and create-fself can't import it
    target_link_options(eden-ps4 PRIVATE --undefined=gethostbyname)
    ps4_add_eboot(eden-ps4)
else()
    add_executable(eden-ps4 main.cpp platform_host.cpp)
endif()

target_link_libraries(eden-ps4 PRIVATE common core audio_core video_core hid_core input_common
    frontend_common network Threads::Threads)
# vma.h (the Vulkan memory allocator, compiled in main.cpp) isn't warning-clean.
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS
    "-Wno-conversion;-Wno-unused-variable;-Wno-unused-parameter;-Wno-missing-field-initializers")
