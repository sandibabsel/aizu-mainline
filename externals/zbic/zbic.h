// SPDX-FileCopyrightText: 2026 aizu-mainline contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Decompresses one ZBIC frame sequence (zstd with "ZBIC" magic and BIC-coded
/// FSE tables, used for NSO segments since Switch SDK 22).
/// Returns the number of bytes written, or (size_t)-1 on error.
size_t aizu_zbic_decompress(void* dst, size_t dst_size, const void* src, size_t src_size);

#ifdef __cplusplus
}
#endif
