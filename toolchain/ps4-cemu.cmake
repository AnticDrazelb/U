# Toolchain for Cemu on PS4: DolphinPS4's love-ps4-style modern toolchain (clang 21 + libc++ 21,
# ld.lld called directly, OpenOrbis CRT/libc), plus this port's sysroot (scripts/build-deps.sh)
# on the find path so find_package() picks up our libraries before PacBrew's.
include(${CMAKE_CURRENT_LIST_DIR}/ps4-love-modern.cmake)

list(PREPEND CMAKE_FIND_ROOT_PATH ${PS4_SYSROOT})
set(CMAKE_TRY_COMPILE_PLATFORM_VARIABLES ${CMAKE_TRY_COMPILE_PLATFORM_VARIABLES} CMAKE_FIND_ROOT_PATH)
# Our sysroot's headers instead of PacBrew's port libraries (usr/include has an old fmt, SDL2, ...):
# a -I there would be searched before any -isystem directory.
foreach(_lang C CXX ASM)
    string(REPLACE "-I${OPENORBIS}/usr/include" "-I${PS4_SYSROOT}/include"
           CMAKE_${_lang}_FLAGS_INIT "${CMAKE_${_lang}_FLAGS_INIT}")
endforeach()
