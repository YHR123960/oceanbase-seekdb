/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace oceanbase::sql::file_sql::parquet_io
{

// This API carries only scalars, pointers and function pointers across the
// boundary. The vendor Arrow build and seekdb use different libstdc++ ABIs.
struct Scan;

enum class Result { OK, END, INVALID, UNSUPPORTED, IO };
enum class Scalar { NIL, INTEGER, REAL, BOOL, STRING, DATE, DATETIME };

using FieldCallback = bool (*)(void *, const char *, size_t, Scalar, bool, uint8_t);
using ValueCallback = bool (*)(void *, const char *, size_t, bool);

Result open(int owned_fd, Scan *&scan) noexcept;
Result schema(Scan &scan, void *ctx, FieldCallback field) noexcept;
Result start(Scan &scan, const int64_t *projection, size_t count) noexcept;
Result next(Scan &scan, void *ctx, ValueCallback value) noexcept;
void close(Scan *scan) noexcept;

} // namespace oceanbase::sql::file_sql::parquet_io
