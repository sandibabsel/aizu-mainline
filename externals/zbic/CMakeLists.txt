# SPDX-FileCopyrightText: 2026 aizu-mainline contributors
# SPDX-License-Identifier: GPL-2.0-or-later

# ZBIC decompressor for NSO segments of games built with Switch SDK 22+
enable_language(C)
find_package(Threads REQUIRED)

add_library(zbic STATIC zbic.c)
target_include_directories(zbic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zbic PRIVATE Threads::Threads)
set_target_properties(zbic PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
if (MSVC)
    target_compile_options(zbic PRIVATE /w)
else()
    target_compile_options(zbic PRIVATE -w)
endif()
