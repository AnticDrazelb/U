# Toolchain for Eden on the PS4: Crema's (clang 21, libc++ 21, ld.lld 12, the OpenOrbis C library
# and Crema's sysroot; ../toolchain/ps4-cemu.cmake), plus what Eden's build needs there.
include(${CMAKE_CURRENT_LIST_DIR}/../toolchain/ps4-cemu.cmake)

# Boost.Asio picks kqueue on FreeBSD, whose header the PS4's C library doesn't have.
foreach(_lang C CXX)
    string(APPEND CMAKE_${_lang}_FLAGS_INIT " -DBOOST_ASIO_DISABLE_KQUEUE")
endforeach()

