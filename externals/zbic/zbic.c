// SPDX-FileCopyrightText: Copyright (c) Atmosphere-NX
// SPDX-FileCopyrightText: 2026 aizu-mainline contributors
// SPDX-License-Identifier: GPL-2.0-or-later
//
// zstd.inc / zstd.h / zstd_errors.h are taken unmodified from Atmosphere
// (libraries/libstratosphere/source/util, commit 36cc9a9f): zstd 1.5.7 built with
// ZSTD_ZBIC_SUPPORT, which changes the frame magic to "ZBIC" and reads FSE
// tables with binary interpolative coding. Based on util_compression_zstd_bic.cpp.

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "zbic_rename.h"

// zstd.inc defines _GNU_SOURCE after zstd.h already pulled in the libc headers,
// so its COVER dictionary builder may call a qsort_r() that libc never declared
// (or that does not exist at all on Windows/older bionic). The dictionary builder
// is never used here; route it to a small local sort so the file builds everywhere.
#if !defined(__APPLE__)
static void aizu_zbic_qsort_r(void* base, size_t count, size_t size,
                              int (*compare)(const void*, const void*, void*), void* context) {
    unsigned char* bytes = (unsigned char*)base;
    unsigned char tmp[64];
    if (size > sizeof(tmp)) {
        return;
    }
    for (size_t i = 1; i < count; ++i) {
        for (size_t j = i; j > 0 && compare(bytes + (j - 1) * size, bytes + j * size, context) > 0;
             --j) {
            memcpy(tmp, bytes + j * size, size);
            memcpy(bytes + j * size, bytes + (j - 1) * size, size);
            memcpy(bytes + (j - 1) * size, tmp, size);
        }
    }
}
#define qsort_r aizu_zbic_qsort_r
#endif

#define ZSTD_STATIC_LINKING_ONLY
#define ZSTD_ZBIC_SUPPORT 1
#define ZSTDLIB_VISIBLE static
#define ZSTDLIB_HIDDEN static

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#endif

#include "zstd.h"
#include "zstd.inc"

#include "zbic.h"

size_t aizu_zbic_decompress(void* dst, size_t dst_size, const void* src, size_t src_size) {
    const size_t result = ZSTD_decompress(dst, dst_size, src, src_size);
    return ZSTD_isError(result) ? (size_t)-1 : result;
}
