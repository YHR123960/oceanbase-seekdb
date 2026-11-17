/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sql/engine/basic/file_parquet_bridge.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <numeric>
#include <parquet/arrow/reader.h>
#include <parquet/file_reader.h>
#include <unistd.h>

namespace oceanbase::sql::file_sql::parquet_io
{

struct Scan
{
  std::unique_ptr<parquet::arrow::FileReader> file;
  std::unique_ptr<arrow::RecordBatchReader> batches;
  std::shared_ptr<arrow::RecordBatch> batch;
  int64_t offset = 0;
  int64_t unread_rows = 0;
  size_t columns = 0;
};

Result open(int owned_fd, Scan *&scan) noexcept
{
  scan = nullptr;
  try {
    auto input = arrow::io::ReadableFile::Open(owned_fd);
    if (!input.ok()) return Result::IO;
    auto result = std::make_unique<Scan>();
    if (!parquet::arrow::OpenFile(*input, arrow::default_memory_pool(), &result->file).ok()) {
      return Result::INVALID;
    }
    scan = result.release();
    return Result::OK;
  } catch (...) {
    return Result::INVALID;
  }
}

Result schema(Scan &scan, void *ctx, FieldCallback field) noexcept
{
  try {
    std::shared_ptr<arrow::Schema> schema;
    if (!scan.file->GetSchema(&schema).ok() || schema->num_fields() < 1
        || scan.file->parquet_reader()->metadata()->num_columns() != schema->num_fields()) {
      return Result::INVALID;
    }
    for (const auto &column : schema->fields()) {
      Scalar type = Scalar::NIL;
      uint8_t scale = 0;
      switch (column->type()->id()) {
        case arrow::Type::BOOL: type = Scalar::BOOL; break;
        case arrow::Type::INT8:
        case arrow::Type::INT16:
        case arrow::Type::INT32:
        case arrow::Type::INT64:
        case arrow::Type::UINT8:
        case arrow::Type::UINT16:
        case arrow::Type::UINT32: type = Scalar::INTEGER; break;
        case arrow::Type::FLOAT:
        case arrow::Type::DOUBLE: type = Scalar::REAL; break;
        case arrow::Type::STRING:
        case arrow::Type::LARGE_STRING: type = Scalar::STRING; break;
        case arrow::Type::DATE32: type = Scalar::DATE; break;
        case arrow::Type::TIMESTAMP: {
          const auto &ts = static_cast<const arrow::TimestampType &>(*column->type());
          if (!ts.timezone().empty() || ts.unit() == arrow::TimeUnit::NANO) return Result::UNSUPPORTED;
          scale = ts.unit() == arrow::TimeUnit::MICRO ? 6 : ts.unit() == arrow::TimeUnit::MILLI ? 3 : 0;
          type = Scalar::DATETIME;
          break;
        }
        case arrow::Type::NA: break;
        default: return Result::UNSUPPORTED;
      }
      const auto &name = column->name();
      if (!field(ctx, name.data(), name.size(), type, column->nullable() || type == Scalar::NIL, scale)) {
        return Result::INVALID;
      }
    }
    return Result::OK;
  } catch (...) {
    return Result::INVALID;
  }
}

Result start(Scan &scan, const int64_t *projection, size_t count) noexcept
{
  try {
    scan.columns = count;
    if (count == 0) {
      scan.unread_rows = scan.file->parquet_reader()->metadata()->num_rows();
      return scan.unread_rows < 0 ? Result::INVALID : Result::OK;
    }
    std::vector<int> groups(scan.file->num_row_groups());
    std::iota(groups.begin(), groups.end(), 0);
    const std::vector<int> columns(projection, projection + count);
    scan.file->set_batch_size(128);
    return scan.file->GetRecordBatchReader(groups, columns, &scan.batches).ok()
        ? Result::OK : Result::INVALID;
  } catch (...) {
    return Result::INVALID;
  }
}

Result next(Scan &scan, void *ctx, ValueCallback value) noexcept
{
  try {
    if (scan.columns == 0) {
      if (scan.unread_rows == 0) return Result::END;
      --scan.unread_rows;
      return Result::OK;
    }
    while (scan.batch == nullptr || scan.offset == scan.batch->num_rows()) {
      if (!scan.batches->ReadNext(&scan.batch).ok()) return Result::INVALID;
      if (scan.batch == nullptr) return Result::END;
      if (scan.batch->num_columns() != static_cast<int>(scan.columns)) return Result::INVALID;
      scan.offset = 0;
    }
    for (size_t i = 0; i < scan.columns; ++i) {
      auto cell = scan.batch->column(i)->GetScalar(scan.offset);
      if (!cell.ok()) return Result::INVALID;
      if (!(*cell)->is_valid) {
        if (!value(ctx, nullptr, 0, true)) return Result::INVALID;
      } else {
        const std::string text = (*cell)->ToString();
        if (!value(ctx, text.data(), text.size(), false)) return Result::INVALID;
      }
    }
    ++scan.offset;
    return Result::OK;
  } catch (...) {
    return Result::INVALID;
  }
}

void close(Scan *scan) noexcept
{
  delete scan;
}

} // namespace oceanbase::sql::file_sql::parquet_io
