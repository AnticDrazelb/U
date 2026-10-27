# The Eden PS4 launcher, built inside Eden's tree with EDEN_EXTRA_FRONTEND_DIR (see ../build.sh).
add_executable(eden-ps4 main.cpp platform_host.cpp)
target_link_libraries(eden-ps4 PRIVATE common core audio_core video_core hid_core input_common
    frontend_common network Threads::Threads)
# vma.h (the Vulkan memory allocator, compiled in main.cpp) isn't warning-clean.
set_source_files_properties(main.cpp PROPERTIES COMPILE_OPTIONS
    "-Wno-conversion;-Wno-unused-variable;-Wno-unused-parameter;-Wno-missing-field-initializers")
