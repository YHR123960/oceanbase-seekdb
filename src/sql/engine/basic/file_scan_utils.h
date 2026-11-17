/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase
{
namespace sql
{
namespace file_sql
{

enum class Kind : uint8_t { CSV, JSONL, PARQUET, GLOB };
enum class Type : uint8_t { NIL, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
enum class Status { OK, END, DENIED, IO, INVALID, CHANGED, UNSUPPORTED, EXISTS };

struct Options
{
  Kind kind = Kind::CSV;
  bool header = true;
  char delimiter = ',';
  char quote = '"';
  char escape = '"';
  std::string null_text = "NULL";
};

struct Cell
{
  Type type = Type::NIL;
  std::string text;
};

struct Column
{
  std::string name;
  std::string key;
  Type type = Type::NIL;
  bool nullable = false;
  uint8_t temporal_scale = 0;
};

struct FileStamp
{
  int64_t dev = 0;
  int64_t ino = 0;
  int64_t size = 0;
  int64_t mtime = 0;
  int64_t mtime_nsec = 0;
  int64_t ctime = 0;
  int64_t ctime_nsec = 0;
  bool operator==(const FileStamp &other) const;
};

struct ScanSpec
{
  Options options;
  bool describe = false;
  std::string path;
  FileStamp stamp;
  std::vector<Column> columns;
  const std::vector<Column> &output_columns() const;
  std::string encode() const;
  bool decode(const char *bytes, size_t len);
};

const char *type_name(Type type);
Type widen(Type left, Type right);
bool parse_integer(const std::string &text, int64_t &out);
bool parse_double(const std::string &text, double &out);
bool parse_temporal(const std::string &text, int64_t &micros);
bool valid_options(const Options &options);

class Reader
{
public:
  Reader();
  ~Reader();
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;

  Status infer(const std::string &root, ScanSpec &spec,
               const std::function<bool()> &continue_scan = {});
  Status open(const std::string &root, const ScanSpec &spec,
              const std::vector<int64_t> &projection);
  Status next(std::vector<Cell> &row);
  void close();
  const std::string &error() const { return error_; }

private:
  Status open_file(const std::string &root, const ScanSpec &spec, bool infer);
  Status check_file();
  Status csv_record(std::vector<Cell> &row, bool header);
  Status json_record(std::vector<Column> &keys, std::vector<Cell> &row);
  Status fail(Status status, const char *message);
  void close_file();
  Status open_parquet(std::vector<Column> *schema);
  Status parquet_row(std::vector<Cell> &row);
  struct ParquetState;
  std::unique_ptr<ParquetState> parquet_;

  ScanSpec spec_;
  FILE *file_ = nullptr;
  int dir_ = -1;
  std::string leaf_;
  std::string error_;
  uint64_t line_ = 1;
  uint64_t record_line_ = 1;
  std::vector<std::string> paths_;
  size_t path_idx_ = 0;
  std::vector<int64_t> projection_;
};

class CsvWriter
{
public:
  CsvWriter() = default;
  ~CsvWriter();
  CsvWriter(const CsvWriter &) = delete;
  CsvWriter &operator=(const CsvWriter &) = delete;

  Status open(const std::string &root, const std::string &path, const Options &options);
  Status append(const std::vector<Cell> &row);
  Status publish();

private:
  int directory_ = -1;
  FILE *file_ = nullptr;
  std::string destination_;
  std::string temporary_;
  Options options_;
};

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
