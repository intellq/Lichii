# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# nxformat (https://github.com/fab918/nxformat) reads and writes .nx game
# archives. The core uses its header-only C++ reader, vendored in
# externals/nxformat. The Qt frontend's NX converter calls its C library to
# convert games to .nx and to edit them. The pinned release of that library is
# downloaded and checked here; LICHII_NXFORMAT_ROOT may instead point at an
# unpacked release, such as a local build packed by nxformat's
# tools/package_release.py. When neither is available, the frontend is built
# without the converter.
#
# The static library is linked into the executable, so there is no shared
# library to ship: a missing one stopped Lichii from starting. Its variant
# without Zstandard is used, so the converter runs on Lichii's own bundled zstd
# and the executable holds a single copy of it.
#
# Defines nxformat::static_nozstd when the converter is built.
set(LICHII_NXFORMAT_ROOT "" CACHE PATH "Unpacked nxformat release (empty: download the pinned one)")

function(lichii_find_nxformat)
    set(version 1.0.2)
    set(tag V1.0.2)
    set(root "${LICHII_NXFORMAT_ROOT}")
    if (NOT root)
        if (WIN32 AND ARCHITECTURE_x86_64)
            set(platform windows-x64)
            set(sha256 5e6ef7dd2878f832c7ec90bd6ffa78f759b05a10fdf37997e2aed9a9ac662983)
        elseif (APPLE)
            set(platform macos-universal)
            set(sha256 4cdd2e39d6e42aa123da1c8c666766db2326a4eb1f2621f6d7de9e68e775a349)
        elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_x86_64)
            set(platform linux-x64)
            set(sha256 da94373e71b19e87459a3d6e0bc8f75a47ca405e1e73cdf87373ace71b97d95b)
        elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux" AND ARCHITECTURE_arm64)
            set(platform linux-arm64)
            set(sha256 be5e9e843d5efb7da48a5e54e3b4d416000d9375f038a7d0aa31b3af63e16ac2)
        else()
            message(WARNING "NX converter disabled: nxformat has no prebuilt library for this platform. "
                            "Set LICHII_NXFORMAT_ROOT to an unpacked nxformat release to build it. "
                            "Loading .nx games is not affected.")
            return()
        endif()
        if (WIN32)
            set(ext zip)
        else()
            set(ext tar.gz)
        endif()
        set(name "nxformat-${version}-${platform}")
        set(root "${CMAKE_BINARY_DIR}/externals/${name}")
        if (NOT EXISTS "${root}/lib/cmake/nxformat/nxformatConfig.cmake")
            set(url "https://github.com/fab918/nxformat/releases/download/${tag}/${name}.${ext}")
            set(archive "${CMAKE_BINARY_DIR}/externals/${name}.${ext}")
            # The hash is checked here rather than by file(DOWNLOAD), which reports a failed
            # download as a configure error.
            file(DOWNLOAD "${url}" "${archive}" TLS_VERIFY ON STATUS status)
            list(GET status 0 code)
            if (code EQUAL 0)
                file(SHA256 "${archive}" actual)
                if (NOT actual STREQUAL sha256)
                    set(code 1)
                    set(status "${code};SHA-256 ${actual} does not match the pinned ${sha256}")
                endif()
            endif()
            if (NOT code EQUAL 0)
                list(GET status 1 reason)
                file(REMOVE "${archive}")
                message(WARNING "NX converter disabled: could not download ${url} (${reason}). "
                                "Reconfigure with network access, or set LICHII_NXFORMAT_ROOT to an "
                                "unpacked nxformat release, to build it. Loading .nx games is not affected.")
                return()
            endif()
            file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${root}")
        endif()
    endif()

    find_package(nxformat ${version} REQUIRED CONFIG PATHS "${root}/lib/cmake/nxformat" NO_DEFAULT_PATH)
    set_target_properties(nxformat::static_nozstd PROPERTIES IMPORTED_GLOBAL TRUE)
    set_property(TARGET nxformat::static_nozstd APPEND PROPERTY INTERFACE_LINK_LIBRARIES zstd::zstd)
    if (MSVC)
        # The library is built against the release DLL runtime (/MD); Debug builds use /MDd.
        set_property(TARGET nxformat::static_nozstd APPEND PROPERTY INTERFACE_LINK_OPTIONS
            "$<$<CONFIG:Debug>:/NODEFAULTLIB:msvcrt>")
    endif()
    message(STATUS "nxformat ${nxformat_VERSION}: ${root}")
endfunction()

if (ENABLE_QT)
    lichii_find_nxformat()
endif()
