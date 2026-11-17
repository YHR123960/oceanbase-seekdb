/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "sql/engine/basic/file_scan_utils.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#if defined(SEEKDB_FILE_SQL_ARROW) && !defined(_WIN32)
#include "sql/engine/basic/file_parquet_bridge.h"
#endif
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#ifndef _WIN32
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oceanbase
{
namespace sql
{
namespace file_sql
{
namespace
{

constexpr size_t MAX_RECORD = 16 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
constexpr size_t MAX_PATHS = 65536;
constexpr size_t MAX_PATH = 4096;
constexpr size_t MAX_NAME = 128;

bool digit(char c)
{
  return c >= '0' && c <= '9';
}

std::string folded(std::string name)
{
  for (char &c : name) {
    if (c >= 'A' && c <= 'Z') {
      c += 'a' - 'A';
    }
  }
  return name;
}

bool utf8(std::string_view text)
{
  for (size_t i = 0; i < text.size();) {
    const unsigned char first = text[i++];
    if (first < 0x80) {
      continue;
    }
    const int extra = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc2 ? 1 : 0;
    if (extra == 0 || first > 0xf4 || i + extra > text.size()) {
      return false;
    }
    const unsigned char second = text[i];
    if ((first == 0xe0 && second < 0xa0) || (first == 0xed && second >= 0xa0)
        || (first == 0xf0 && second < 0x90) || (first == 0xf4 && second >= 0x90)) {
      return false;
    }
    for (int j = 0; j < extra; ++j) {
      if ((static_cast<unsigned char>(text[i++]) & 0xc0) != 0x80) {
        return false;
      }
    }
  }
  return true;
}

void append_utf8(std::string &out, uint32_t value)
{
  if (value < 0x80) {
    out.push_back(static_cast<char>(value));
  } else {
    const int count = value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
    out.push_back(static_cast<char>((0xf00u >> count) | (value >> (6 * (count - 1)))));
    for (int i = count - 2; i >= 0; --i) {
      out.push_back(static_cast<char>(0x80 | ((value >> (i * 6)) & 0x3f)));
    }
  }
}

Type text_type(const std::string &text, bool string_only)
{
  int64_t integer = 0;
  double real = 0;
  if (parse_temporal(text, integer)) {
    return text.size() == 10 ? Type::DATE : Type::DATETIME;
  }
  if (string_only) {
    return Type::VARCHAR;
  }
  if (folded(text) == "true" || folded(text) == "false") {
    return Type::BOOLEAN;
  }
  if (parse_integer(text, integer)) {
    return Type::BIGINT;
  }
  if (text.find_first_of(".eE") != std::string::npos && parse_double(text, real)) {
    return Type::DOUBLE;
  }
  return Type::VARCHAR;
}

class FlatJson
{
public:
  explicit FlatJson(std::string_view input) : input_(input) {}

  bool parse(std::vector<Column> &keys, std::vector<Cell> &cells)
  {
    if (!take('{')) {
      return false;
    }
    std::unordered_set<std::string> names;
    if (!take('}')) {
      do {
        Column key;
        Cell cell;
        if (keys.size() >= MAX_COLUMNS || !string(key.name) || key.name.empty()
            || key.name.size() > MAX_NAME || key.name.find('\0') != std::string::npos
            || !names.insert(key.name).second || !take(':') || !value(cell)) {
          return false;
        }
        key.key = key.name;
        keys.push_back(std::move(key));
        cells.push_back(std::move(cell));
      } while (take(','));
      if (!take('}')) {
        return false;
      }
    }
    space();
    return pos_ == input_.size();
  }

private:
  void space()
  {
    while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t'
           || input_[pos_] == '\r' || input_[pos_] == '\n')) {
      ++pos_;
    }
  }

  bool take(char c)
  {
    space();
    if (pos_ < input_.size() && input_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool hex4(uint32_t &value)
  {
    value = 0;
    for (int i = 0; i < 4; ++i) {
      if (pos_ == input_.size()) {
        return false;
      }
      const char c = input_[pos_++];
      if (digit(c)) value = 16 * value + c - '0';
      else if (c >= 'a' && c <= 'f') value = 16 * value + c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') value = 16 * value + c - 'A' + 10;
      else return false;
    }
    return true;
  }

  bool string(std::string &out)
  {
    if (!take('"')) {
      return false;
    }
    while (pos_ < input_.size()) {
      const unsigned char c = input_[pos_++];
      if (c == '"') return utf8(out);
      if (c < 0x20) return false;
      if (c != '\\') {
        out.push_back(c);
      } else if (pos_ == input_.size()) {
        return false;
      } else {
        switch (input_[pos_++]) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            uint32_t cp = 0;
            if (!hex4(cp)) return false;
            if (cp >= 0xd800 && cp <= 0xdbff) {
              if (input_.substr(pos_, 2) != "\\u") return false;
              pos_ += 2;
              uint32_t low = 0;
              if (!hex4(low) || low < 0xdc00 || low > 0xdfff) return false;
              cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
              return false;
            }
            append_utf8(out, cp);
            break;
          }
          default: return false;
        }
      }
    }
    return false;
  }

  bool digits()
  {
    const size_t start = pos_;
    while (pos_ < input_.size() && digit(input_[pos_])) ++pos_;
    return start != pos_;
  }

  bool value(Cell &cell)
  {
    space();
    if (pos_ == input_.size()) return false;
    if (input_[pos_] == '"') {
      if (!string(cell.text)) return false;
      cell.type = text_type(cell.text, true);
      return true;
    }
    for (const std::string_view literal : {"null", "true", "false"}) {
      if (input_.substr(pos_, literal.size()) == literal) {
        pos_ += literal.size();
        if (literal != "null") {
          cell.type = Type::BOOLEAN;
          cell.text.assign(literal);
        }
        return true;
      }
    }
    const size_t start = pos_;
    if (input_[pos_] == '-') ++pos_;
    if (pos_ < input_.size() && input_[pos_] == '0') ++pos_;
    else if (!digits()) return false;
    bool floating = pos_ < input_.size() && input_[pos_] == '.';
    if (floating) {
      ++pos_;
      if (!digits()) return false;
    }
    if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
      floating = true;
      ++pos_;
      if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) ++pos_;
      if (!digits()) return false;
    }
    cell.text.assign(input_.substr(start, pos_ - start));
    int64_t integer = 0;
    double real = 0;
    cell.type = floating ? (parse_double(cell.text, real) ? Type::DOUBLE : Type::VARCHAR)
                         : (parse_integer(cell.text, integer) ? Type::BIGINT : Type::VARCHAR);
    return true;
  }

  std::string_view input_;
  size_t pos_ = 0;
};

#ifndef _WIN32
FileStamp stamp_of(const struct stat &info)
{
#ifdef __APPLE__
  const auto mtime = info.st_mtimespec;
  const auto ctime = info.st_ctimespec;
#else
  const auto mtime = info.st_mtim;
  const auto ctime = info.st_ctim;
#endif
  return {static_cast<int64_t>(info.st_dev), static_cast<int64_t>(info.st_ino),
          static_cast<int64_t>(info.st_size), mtime.tv_sec, mtime.tv_nsec,
          ctime.tv_sec, ctime.tv_nsec};
}

// Walk from the root by descriptor. This rejects symlink components even
// when an attacker replaces a directory after it has been opened.
Status parent_directory(const std::string &root, const std::string &path,
                        int &parent, std::string &leaf)
{
  if (root.empty() || root[0] != '/' || path.empty() || path[0] != '/'
      || path.size() >= MAX_PATH || path.find('\0') != std::string::npos
      || root.find('\0') != std::string::npos) {
    return Status::DENIED;
  }
  std::string base = root;
  while (base.size() > 1 && base.back() == '/') base.pop_back();
  if (base != "/" && (path.compare(0, base.size(), base) != 0
      || path.size() <= base.size() || path[base.size()] != '/')) {
    return Status::DENIED;
  }
  parent = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (parent < 0) return Status::IO;
  size_t start = 1;
  while (start < path.size()) {
    const size_t end = path.find('/', start);
    leaf = path.substr(start, end == std::string::npos ? end : end - start);
    if (leaf.empty() || leaf == "." || leaf == "..") {
      ::close(parent);
      parent = -1;
      return Status::DENIED;
    }
    if (end == std::string::npos) return Status::OK;
    const int child = ::openat(parent, leaf.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(parent);
    parent = child;
    if (child < 0) return Status::DENIED;
    start = end + 1;
  }
  ::close(parent);
  parent = -1;
  return Status::DENIED;
}
#endif

void put_u64(std::string &out, uint64_t value)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<char>(value & 0xff));
    value >>= 8;
  }
}

bool take_u64(std::string_view &bytes, uint64_t &out)
{
  if (bytes.size() < 8) return false;
  out = 0;
  for (int i = 0; i < 8; ++i) {
    out |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[i])) << (i * 8);
  }
  bytes.remove_prefix(8);
  return true;
}

void put_string(std::string &out, const std::string &str)
{
  put_u64(out, str.size());
  out.append(str);
}

bool take_string(std::string_view &bytes, std::string &out, size_t limit)
{
  uint64_t length = 0;
  if (!take_u64(bytes, length) || length > bytes.size() || length > limit) return false;
  out.assign(bytes.substr(0, length));
  bytes.remove_prefix(length);
  return true;
}

} // namespace

const char *type_name(Type type)
{
  switch (type) {
    case Type::BIGINT: return "BIGINT";
    case Type::DOUBLE: return "DOUBLE";
    case Type::BOOLEAN: return "BOOLEAN";
    case Type::VARCHAR: return "VARCHAR";
    case Type::DATE: return "DATE";
    case Type::DATETIME: return "DATETIME";
    default: return "NULL";
  }
}

Type widen(Type left, Type right)
{
  if (left == right || right == Type::NIL) return left;
  if (left == Type::NIL) return right;
  if ((left == Type::BIGINT && right == Type::DOUBLE)
      || (left == Type::DOUBLE && right == Type::BIGINT)) return Type::DOUBLE;
  if ((left == Type::DATE && right == Type::DATETIME)
      || (left == Type::DATETIME && right == Type::DATE)) return Type::DATETIME;
  return Type::VARCHAR;
}

bool parse_integer(const std::string &text, int64_t &out)
{
  const char *start = text.data();
  const char *end = start + text.size();
  if (start != end && *start == '+') {
    ++start;
    if (start == end || !digit(*start)) return false;
  }
  const auto r = std::from_chars(start, end, out);
  return r.ec == std::errc{} && r.ptr == end;
}

bool parse_double(const std::string &text, double &out)
{
  const char *start = text.data();
  const char *end = start + text.size();
  if (start != end && *start == '+') {
    ++start;
    if (start == end || (!digit(*start) && *start != '.')) return false;
  }
  const auto r = std::from_chars(start, end, out);
  return r.ec == std::errc{} && r.ptr == end && std::isfinite(out);
}

bool parse_temporal(const std::string &text, int64_t &micros)
{
  if (text.size() != 10 && (text.size() < 19 || text.size() > 26)) return false;
  const auto number = [&text](size_t pos, size_t count) {
    int value = 0;
    for (size_t i = pos; i < pos + count; ++i) {
      if (!digit(text[i])) return -1;
      value = 10 * value + text[i] - '0';
    }
    return value;
  };
  int year = number(0, 4);
  const int month = number(5, 2);
  const int day = number(8, 2);
  if (year < 1 || month < 1 || month > 12 || text[4] != '-' || text[7] != '-') return false;
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const int days_in_month[] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (day < 1 || day > days_in_month[month - 1]) return false;
  int hour = 0, minute = 0, second = 0, fraction = 0;
  if (text.size() != 10) {
    hour = number(11, 2);
    minute = number(14, 2);
    second = number(17, 2);
    if ((text[10] != 'T' && text[10] != ' ') || text[13] != ':' || text[16] != ':'
        || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    if (text.size() > 19) {
      if (text[19] != '.' || text.size() == 20) return false;
      fraction = number(20, text.size() - 20);
      if (fraction < 0) return false;
      for (size_t i = text.size(); i < 26; ++i) fraction *= 10;
    }
  }
  year -= month <= 2;
  const int era = year / 400;
  const int in_era = year - era * 400;
  const int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t days = era * 146097 + in_era * 365 + in_era / 4 - in_era / 100 + day_of_year - 719468;
  micros = (((days * 24 + hour) * 60 + minute) * 60 + second) * 1000000 + fraction;
  return true;
}

bool valid_options(const Options &options)
{
  const auto special = [](char c) { return c <= 0 || c == '\n' || c == '\r'; };
  return !special(options.delimiter) && !special(options.quote) && !special(options.escape)
      && options.delimiter != options.quote && options.delimiter != options.escape
      && options.null_text.size() <= 1024 && utf8(options.null_text);
}

bool FileStamp::operator==(const FileStamp &other) const
{
  return dev == other.dev && ino == other.ino && size == other.size
      && mtime == other.mtime && mtime_nsec == other.mtime_nsec
      && ctime == other.ctime && ctime_nsec == other.ctime_nsec;
}

const std::vector<Column> &ScanSpec::output_columns() const
{
  static const std::vector<Column> description = {
    {"column_name", "", Type::VARCHAR, false}, {"column_type", "", Type::VARCHAR, false},
    {"null", "", Type::VARCHAR, false}, {"key", "", Type::VARCHAR, true},
    {"default", "", Type::VARCHAR, true}, {"extra", "", Type::VARCHAR, true},
  };
  return describe ? description : columns;
}

std::string ScanSpec::encode() const
{
  std::string out;
  out.push_back(1);
  out.push_back(static_cast<char>(options.kind));
  out.push_back(options.header);
  out.push_back(options.delimiter);
  out.push_back(options.quote);
  out.push_back(options.escape);
  out.push_back(describe);
  put_string(out, path);
  put_string(out, options.null_text);
  for (int64_t part : {stamp.dev, stamp.ino, stamp.size, stamp.mtime,
                       stamp.mtime_nsec, stamp.ctime, stamp.ctime_nsec}) {
    put_u64(out, part);
  }
  put_u64(out, columns.size());
  for (const auto &col : columns) {
    put_string(out, col.name);
    put_string(out, col.key);
    out.push_back(static_cast<char>(col.type));
    out.push_back(col.nullable);
    out.push_back(col.temporal_scale);
  }
  return out;
}

bool ScanSpec::decode(const char *input, size_t len)
{
  if (input == nullptr || len < 7 || input[0] != 1
      || static_cast<uint8_t>(input[1]) > static_cast<uint8_t>(Kind::GLOB)) return false;
  options.kind = static_cast<Kind>(input[1]);
  options.header = input[2] != 0;
  options.delimiter = input[3];
  options.quote = input[4];
  options.escape = input[5];
  describe = input[6] != 0;
  std::string_view bytes(input + 7, len - 7);
  uint64_t count = 0;
  if (!take_string(bytes, path, MAX_PATH) || !take_string(bytes, options.null_text, 1024)
      || !valid_options(options)) return false;
  for (int64_t *part : {&stamp.dev, &stamp.ino, &stamp.size, &stamp.mtime,
                        &stamp.mtime_nsec, &stamp.ctime, &stamp.ctime_nsec}) {
    uint64_t value = 0;
    if (!take_u64(bytes, value)) return false;
    *part = static_cast<int64_t>(value);
  }
  if (!take_u64(bytes, count) || count == 0 || count > MAX_COLUMNS) return false;
  columns.clear();
  for (uint64_t i = 0; i < count; ++i) {
    Column col;
    if (!take_string(bytes, col.name, MAX_NAME) || !take_string(bytes, col.key, MAX_NAME)
        || bytes.size() < 3
        || static_cast<uint8_t>(bytes[0]) > static_cast<uint8_t>(Type::DATETIME)
        || static_cast<uint8_t>(bytes[2]) > 6) return false;
    col.type = static_cast<Type>(bytes[0]);
    col.nullable = bytes[1] != 0;
    col.temporal_scale = bytes[2];
    bytes.remove_prefix(3);
    columns.push_back(std::move(col));
  }
  return bytes.empty();
}

struct Reader::ParquetState
{
#if defined(SEEKDB_FILE_SQL_ARROW) && !defined(_WIN32)
  parquet_io::Scan *scan = nullptr;
  ~ParquetState() { parquet_io::close(scan); }
#endif
};

Reader::Reader() = default;
Reader::~Reader() { close(); }

void Reader::close_file()
{
  parquet_.reset();
#ifndef _WIN32
  if (dir_ >= 0) ::close(dir_);
#endif
  dir_ = -1;
  if (file_ != nullptr) std::fclose(file_);
  file_ = nullptr;
}

void Reader::close()
{
  close_file();
  paths_.clear();
  projection_.clear();
  path_idx_ = 0;
  record_line_ = line_ = 1;
}

Status Reader::fail(Status status, const char *message)
{
  error_ = std::string(message) + " (line " + std::to_string(record_line_) + ")";
  return status;
}

Status Reader::open_file(const std::string &root, const ScanSpec &spec, bool infer)
{
  close();
  error_.clear();
  spec_ = spec;
  if (!valid_options(spec.options)) return fail(Status::INVALID, "invalid file format options");
#ifdef _WIN32
  return fail(Status::UNSUPPORTED, "file SQL requires POSIX descriptor-relative access");
#else
  Status status = parent_directory(root, spec.path, dir_, leaf_);
  if (status != Status::OK) return fail(status, "invalid or restricted file path");
  if (spec.options.kind == Kind::GLOB) {
    const size_t slash = spec.path.rfind('/');
    if (spec.path.substr(0, slash).find_first_of("*?[") != std::string::npos) {
      return fail(Status::UNSUPPORTED, "glob wildcards must be in the final path component");
    }
    const int duplicate = ::dup(dir_);
    DIR *directory = duplicate < 0 ? nullptr : ::fdopendir(duplicate);
    if (directory == nullptr) {
      if (duplicate >= 0) ::close(duplicate);
      return fail(Status::IO, "cannot list directory");
    }
    for (;;) {
      errno = 0;
      dirent *entry = ::readdir(directory);
      if (entry == nullptr) {
        if (errno != 0) status = fail(Status::IO, "cannot list directory");
        break;
      }
      struct stat info {};
      if (::fnmatch(leaf_.c_str(), entry->d_name, FNM_PERIOD) == 0
          && ::fstatat(dir_, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0
          && S_ISREG(info.st_mode)) {
        if (paths_.size() >= MAX_PATHS) {
          status = fail(Status::INVALID, "too many glob matches");
          break;
        }
        paths_.push_back(spec.path.substr(0, slash + 1) + entry->d_name);
      }
    }
    ::closedir(directory);
    std::sort(paths_.begin(), paths_.end());
    return status;
  }
  const int fd = ::openat(dir_, leaf_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return fail(errno == ELOOP ? Status::DENIED : Status::IO, "cannot open input file");
  struct stat info {};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
    ::close(fd);
    return fail(Status::DENIED, "input is not a regular file");
  }
  if (infer) spec_.stamp = stamp_of(info);
  else if (!(spec_.stamp == stamp_of(info))) {
    ::close(fd);
    return fail(Status::CHANGED, "input changed since schema inference");
  }
  if ((file_ = ::fdopen(fd, "rb")) == nullptr) {
    ::close(fd);
    return fail(Status::IO, "cannot initialize file stream");
  }
  if (spec.options.kind == Kind::PARQUET) return Status::OK;
  // Skip a UTF-8 BOM only at the start of the file.
  unsigned char bom[3];
  if (std::fread(bom, 1, sizeof(bom), file_) != sizeof(bom)
      || bom[0] != 0xef || bom[1] != 0xbb || bom[2] != 0xbf) {
    if (std::fseek(file_, 0, SEEK_SET) != 0) return fail(Status::IO, "cannot seek input file");
  }
  return Status::OK;
#endif
}

Status Reader::check_file()
{
#ifndef _WIN32
  struct stat opened {}, named {};
  if (file_ == nullptr || ::fstat(::fileno(file_), &opened) != 0
      || ::fstatat(dir_, leaf_.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0
      || !(stamp_of(opened) == spec_.stamp) || !(stamp_of(named) == spec_.stamp)) {
    return fail(Status::CHANGED, "input changed during scan");
  }
#endif
  return Status::OK;
}

Status Reader::open_parquet(std::vector<Column> *schema)
{
#if defined(SEEKDB_FILE_SQL_ARROW) && !defined(_WIN32)
  using parquet_io::Result;
  parquet_ = std::make_unique<ParquetState>();
  const int copy = ::fcntl(::fileno(file_), F_DUPFD_CLOEXEC, 0);
  if (copy < 0) return fail(Status::IO, "cannot duplicate Parquet descriptor");
  const Result opened = parquet_io::open(copy, parquet_->scan);
  if (opened != Result::OK) {
    return fail(opened == Result::IO ? Status::IO : Status::INVALID, "cannot open Parquet footer");
  }
  if (schema != nullptr) {
    struct FieldContext
    {
      std::vector<Column> &schema;
      std::unordered_set<std::string> names;
    } context{*schema, {}};
    const auto add_field = [](void *opaque, const char *bytes, size_t length,
                              parquet_io::Scalar input, bool nullable, uint8_t scale) {
      auto &ctx = *static_cast<FieldContext *>(opaque);
      std::string name(bytes, length);
      if (ctx.schema.size() >= MAX_COLUMNS || name.empty() || name.size() > MAX_NAME
          || name.find('\0') != std::string::npos || !utf8(name)
          || !ctx.names.insert(folded(name)).second) return false;
      Type type = Type::NIL;
      switch (input) {
        case parquet_io::Scalar::NIL: break;
        case parquet_io::Scalar::INTEGER: type = Type::BIGINT; break;
        case parquet_io::Scalar::REAL: type = Type::DOUBLE; break;
        case parquet_io::Scalar::BOOL: type = Type::BOOLEAN; break;
        case parquet_io::Scalar::STRING: type = Type::VARCHAR; break;
        case parquet_io::Scalar::DATE: type = Type::DATE; break;
        case parquet_io::Scalar::DATETIME: type = Type::DATETIME; break;
      }
      ctx.schema.push_back({name, name, type, nullable, scale});
      return true;
    };
    const Result result = parquet_io::schema(*parquet_->scan, &context, add_field);
    if (result == Result::UNSUPPORTED) return fail(Status::UNSUPPORTED, "unsupported Parquet column type");
    if (result != Result::OK) return fail(Status::INVALID, "invalid Parquet schema");
  } else if (parquet_io::start(*parquet_->scan, projection_.data(), projection_.size()) != Result::OK) {
    return fail(Status::INVALID, "cannot read Parquet row groups");
  }
  return Status::OK;
#else
  (void)schema;
  return fail(Status::UNSUPPORTED, "seekdb was built without the Arrow Parquet reader");
#endif
}

Status Reader::parquet_row(std::vector<Cell> &row)
{
#if defined(SEEKDB_FILE_SQL_ARROW) && !defined(_WIN32)
  struct ValueContext
  {
    const ScanSpec &spec;
    const std::vector<int64_t> &projection;
    std::vector<Cell> &row;
  } context{spec_, projection_, row};
  const auto add_value = [](void *opaque, const char *bytes, size_t length, bool is_null) {
    auto &ctx = *static_cast<ValueContext *>(opaque);
    if (ctx.row.size() >= ctx.projection.size() || length > MAX_RECORD) return false;
    Cell cell;
    if (!is_null) {
      cell.type = ctx.spec.columns[ctx.projection[ctx.row.size()]].type;
      cell.text.assign(bytes, length);
      double real = 0;
      int64_t date = 0;
      if (cell.type == Type::VARCHAR && !utf8(cell.text)) return false;
      if (cell.type == Type::DOUBLE && !parse_double(cell.text, real)) return false;
      if ((cell.type == Type::DATE || cell.type == Type::DATETIME)
          && !parse_temporal(cell.text, date)) return false;
    }
    ctx.row.push_back(std::move(cell));
    return true;
  };
  record_line_ = line_++;
  const auto result = parquet_io::next(*parquet_->scan, &context, add_value);
  if (result == parquet_io::Result::END) return Status::END;
  if (result != parquet_io::Result::OK) return fail(Status::INVALID, "cannot decode Parquet row");
  return Status::OK;
#else
  (void)row;
  return fail(Status::UNSUPPORTED, "seekdb was built without the Arrow Parquet reader");
#endif
}

Status Reader::csv_record(std::vector<Cell> &row, bool header)
{
  enum class State { START, UNQUOTED, QUOTED, CLOSED };
  State state = State::START;
  row.clear();
  record_line_ = line_;
  std::string field;
  bool quoted = false;
  bool seen = false;
  size_t bytes = 0;
  const Options &opt = spec_.options;
  const auto finish = [&]() {
    if (row.size() >= MAX_COLUMNS || !utf8(field)) return false;
    const Type type = header ? Type::VARCHAR
                    : !quoted && (field.empty() || field == opt.null_text) ? Type::NIL
                    : text_type(field, false);
    row.push_back({type, std::move(field)});
    field.clear();
    quoted = false;
    return true;
  };
  for (;;) {
    const int c = std::fgetc(file_);
    if (c == EOF) {
      if (std::ferror(file_)) return fail(Status::IO, "cannot read CSV");
      if (state == State::QUOTED) return fail(Status::INVALID, "unterminated CSV quote");
      if (!seen) return Status::END;
      return finish() ? Status::OK : fail(Status::INVALID, "invalid CSV field");
    }
    seen = true;
    if (++bytes > MAX_RECORD) return fail(Status::INVALID, "CSV record exceeds size limit");
    if (state == State::QUOTED) {
      if (c == opt.escape && opt.escape != opt.quote) {
        const int escaped = std::fgetc(file_);
        if (++bytes > MAX_RECORD || (escaped != opt.quote && escaped != opt.escape)) {
          return fail(Status::INVALID, "invalid CSV escape");
        }
        field.push_back(static_cast<char>(escaped));
      } else if (c == opt.quote) {
        state = State::CLOSED;
      } else {
        if (c == '\n') ++line_;
        field.push_back(static_cast<char>(c));
      }
    } else if (state == State::CLOSED && c == opt.quote && opt.escape == opt.quote) {
      field.push_back(opt.quote);
      state = State::QUOTED;
    } else if (c == opt.delimiter) {
      if (!finish()) return fail(Status::INVALID, "invalid CSV field");
      state = State::START;
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && std::fgetc(file_) != '\n') return fail(Status::INVALID, "expected CRLF");
      ++line_;
      return finish() ? Status::OK : fail(Status::INVALID, "invalid CSV field");
    } else if (state == State::START && c == opt.quote) {
      quoted = true;
      state = State::QUOTED;
    } else if (state == State::CLOSED || c == opt.quote) {
      return fail(Status::INVALID, "unexpected character around CSV quote");
    } else {
      field.push_back(static_cast<char>(c));
      state = State::UNQUOTED;
    }
  }
}

Status Reader::json_record(std::vector<Column> &keys, std::vector<Cell> &row)
{
  std::string line;
  do {
    line.clear();
    record_line_ = line_;
    int c = 0;
    while ((c = std::fgetc(file_)) != EOF && c != '\n') {
      if (line.size() >= MAX_RECORD) return fail(Status::INVALID, "JSONL record exceeds size limit");
      line.push_back(static_cast<char>(c));
    }
    if (c == EOF && std::ferror(file_)) return fail(Status::IO, "cannot read JSONL");
    if (c == EOF && line.empty()) return Status::END;
    ++line_;
  } while (line.find_first_not_of(" \t\r") == std::string::npos);
  keys.clear();
  row.clear();
  return FlatJson(line).parse(keys, row) ? Status::OK : fail(Status::INVALID, "expected a flat JSON object");
}

Status Reader::infer(const std::string &root, ScanSpec &spec,
                     const std::function<bool()> &continue_scan)
{
  spec.columns.clear();
  Status status = open_file(root, spec, true);
  if (status == Status::OK && spec.options.kind == Kind::GLOB) {
    spec.columns = {{"file", "file", Type::VARCHAR, false}};
  } else if (status == Status::OK && spec.options.kind == Kind::PARQUET) {
    status = open_parquet(&spec.columns);
    if (status == Status::OK && (status = check_file()) == Status::OK) spec.stamp = spec_.stamp;
  } else if (status == Status::OK) {
    std::vector<Cell> row;
    std::vector<Column> keys;
    std::unordered_map<std::string, size_t> positions;
    std::unordered_set<std::string> names;
    const auto add_column = [&](std::string name, const std::string &key, bool nullable) {
      if (spec.columns.size() >= MAX_COLUMNS || name.size() > MAX_NAME
          || name.find('\0') != std::string::npos) return false;
      if (name.empty()) name = "column" + std::to_string(spec.columns.size() + 1);
      const std::string base = name;
      for (size_t suffix = 2; !names.insert(folded(name)).second; ++suffix) {
        name = base + "__" + std::to_string(suffix);
        if (name.size() > MAX_NAME) return false;
      }
      spec.columns.push_back({std::move(name), key, Type::NIL, nullable});
      return true;
    };
    const auto merge_cell = [](Column &column, const Cell &cell) {
      column.type = widen(column.type, cell.type);
      column.nullable |= cell.type == Type::NIL;
      if (cell.type == Type::DATETIME && cell.text.size() > 20) {
        column.temporal_scale = std::max<uint8_t>(column.temporal_scale, cell.text.size() - 20);
      }
    };
    if (spec.options.kind == Kind::CSV && spec.options.header) {
      status = csv_record(row, true);
      for (size_t i = 0; status == Status::OK && i < row.size(); ++i) {
        if (!add_column(row[i].text, "", false)) status = fail(Status::INVALID, "invalid CSV column name");
      }
    }
    size_t rows = 0;
    while (status == Status::OK) {
      if ((rows & 255) == 0 && continue_scan && !continue_scan()) {
        status = fail(Status::IO, "file schema inference interrupted");
        break;
      }
      if (spec.options.kind == Kind::CSV) {
        status = csv_record(row, false);
        if (status == Status::OK && spec.columns.empty()) {
          for (size_t i = 0; status == Status::OK && i < row.size(); ++i) {
            if (!add_column("", "", false)) status = fail(Status::INVALID, "too many CSV columns");
          }
        }
        if (status == Status::OK && row.size() != spec.columns.size()) {
          status = fail(Status::INVALID, "CSV field count differs from schema");
        }
        for (size_t i = 0; status == Status::OK && i < row.size(); ++i) merge_cell(spec.columns[i], row[i]);
      } else {
        status = json_record(keys, row);
        std::vector<bool> present(spec.columns.size(), false);
        for (size_t i = 0; status == Status::OK && i < keys.size(); ++i) {
          auto result = positions.emplace(keys[i].key, spec.columns.size());
          const size_t index = result.first->second;
          if (result.second && !add_column(keys[i].name, keys[i].key, rows > 0)) {
            status = fail(Status::INVALID, "too many JSONL columns");
          } else {
            present.resize(spec.columns.size(), false);
            present[index] = true;
            merge_cell(spec.columns[index], row[i]);
          }
        }
        for (size_t i = 0; i < present.size(); ++i) spec.columns[i].nullable |= !present[i];
      }
      if (status == Status::OK) ++rows;
    }
    if (status == Status::END) {
      if (spec.columns.empty()) status = fail(Status::INVALID, "file has no columns");
      else if ((status = check_file()) == Status::OK) {
        spec.stamp = spec_.stamp;
        if (rows == 0) {
          for (auto &column : spec.columns) column.nullable = true;
        }
      }
    }
  }
  close_file();
  return status;
}

Status Reader::open(const std::string &root, const ScanSpec &spec,
                    const std::vector<int64_t> &projection)
{
  Status status = open_file(root, spec, false);
  for (const int64_t idx : projection) {
    if (idx < 0 || static_cast<size_t>(idx) >= spec.output_columns().size()) {
      status = fail(Status::INVALID, "invalid file column projection");
      break;
    }
  }
  if (status == Status::OK) {
    projection_ = projection;
    if (spec.options.kind == Kind::PARQUET && !spec.describe) {
      status = open_parquet(nullptr);
    } else if (spec.options.kind == Kind::CSV && spec.options.header) {
      std::vector<Cell> header;
      status = csv_record(header, true);
    }
  }
  if (status != Status::OK) close_file();
  return status;
}

Status Reader::next(std::vector<Cell> &row)
{
  row.clear();
  if (spec_.describe) {
    const Status status = spec_.options.kind == Kind::GLOB ? Status::OK : check_file();
    if (status != Status::OK) return status;
    if (path_idx_ == spec_.columns.size()) return Status::END;
    const auto &col = spec_.columns[path_idx_++];
    const Cell info[] = {{Type::VARCHAR, col.name}, {Type::VARCHAR, type_name(col.type)},
                         {Type::VARCHAR, col.nullable ? "YES" : "NO"}, {}, {}, {}};
    for (const int64_t idx : projection_) row.push_back(info[idx]);
    return Status::OK;
  }
  if (spec_.options.kind == Kind::GLOB) {
    if (path_idx_ == paths_.size()) return Status::END;
    if (!projection_.empty()) row.push_back({Type::VARCHAR, paths_[path_idx_]});
    ++path_idx_;
    return Status::OK;
  }
  if (file_ == nullptr) return fail(Status::IO, "file scan is closed");
  std::vector<Cell> cells;
  std::vector<Column> keys;
  Status status = check_file();
  if (status == Status::OK) {
    if (spec_.options.kind == Kind::PARQUET) status = parquet_row(row);
    else status = spec_.options.kind == Kind::CSV ? csv_record(cells, false) : json_record(keys, cells);
  }
  if (status == Status::OK || status == Status::END) {
    const Status check = check_file();
    if (check != Status::OK) return check;
  }
  if (status != Status::OK || spec_.options.kind == Kind::PARQUET) return status;
  if (spec_.options.kind == Kind::CSV && cells.size() != spec_.columns.size()) {
    return fail(Status::INVALID, "CSV field count differs from schema");
  }
  for (const int64_t idx : projection_) {
    if (spec_.options.kind == Kind::CSV) {
      row.push_back(cells[idx]);
    } else {
      const auto &key = spec_.columns[idx].key;
      const auto it = std::find_if(keys.begin(), keys.end(),
                                   [&](const Column &col) { return col.key == key; });
      row.push_back(it == keys.end() ? Cell{} : cells[it - keys.begin()]);
    }
  }
  return Status::OK;
}

CsvWriter::~CsvWriter()
{
#ifndef _WIN32
  if (file_ != nullptr) std::fclose(file_);
  if (directory_ >= 0) {
    if (!temporary_.empty()) ::unlinkat(directory_, temporary_.c_str(), 0);
    ::close(directory_);
  }
#endif
}

Status CsvWriter::open(const std::string &root, const std::string &path, const Options &options)
{
#ifdef _WIN32
  (void)root;
  (void)path;
  (void)options;
  return Status::UNSUPPORTED;
#else
  if (directory_ >= 0 || !valid_options(options)
      || options.null_text.find_first_of(std::string{options.quote, options.delimiter, '\r', '\n'})
         != std::string::npos || options.null_text.find('\0') != std::string::npos) {
    return Status::INVALID;
  }
  options_ = options;
  Status status = parent_directory(root, path, directory_, destination_);
  if (status != Status::OK) return status;
  struct stat info {};
  if (::fstatat(directory_, destination_.c_str(), &info, AT_SYMLINK_NOFOLLOW) == 0) return Status::EXISTS;
  if (errno != ENOENT) return Status::IO;

  std::random_device random;
  for (int attempt = 0; attempt < 16; ++attempt) {
    const std::string name = ".seekdb-copy-" + std::to_string(random()) + "-" + std::to_string(random());
    if (name == destination_) continue;
    const int fd = ::openat(directory_, name.c_str(),
                            O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
      temporary_ = name;
      file_ = ::fdopen(fd, "wb");
      if (file_ == nullptr) ::close(fd);
      return file_ == nullptr ? Status::IO : Status::OK;
    } else if (errno != EEXIST) {
      return Status::IO;
    }
  }
  return Status::IO;
#endif
}

Status CsvWriter::append(const std::vector<Cell> &row)
{
  if (file_ == nullptr || row.empty() || row.size() > MAX_COLUMNS) return Status::INVALID;
  std::string record;
  for (const auto &cell : row) {
    if (&cell != &row.front()) record.push_back(options_.delimiter);
    if (cell.type == Type::NIL) {
      record.append(options_.null_text);
    } else {
      if (cell.text.size() > MAX_RECORD || !utf8(cell.text)) return Status::INVALID;
      const bool quote = cell.text.empty() || cell.text == options_.null_text
          || cell.text.find_first_of(std::string{options_.quote, options_.delimiter, '\r', '\n'})
             != std::string::npos;
      if (quote) record.push_back(options_.quote);
      for (char c : cell.text) {
        if (quote && (c == options_.quote || c == options_.escape)) record.push_back(options_.escape);
        record.push_back(c);
        if (record.size() >= MAX_RECORD) return Status::INVALID;
      }
      if (quote) record.push_back(options_.quote);
    }
    if (record.size() >= MAX_RECORD) return Status::INVALID;
  }
  record.push_back('\n');
  return std::fwrite(record.data(), 1, record.size(), file_) == record.size() ? Status::OK : Status::IO;
}

Status CsvWriter::publish()
{
#ifdef _WIN32
  return Status::UNSUPPORTED;
#else
  if (file_ == nullptr || temporary_.empty()) return Status::INVALID;
  const int closed = std::fclose(file_);
  file_ = nullptr;
  if (closed != 0) return Status::IO;
  // linkat creates the destination atomically and fails if it exists, including
  // when another writer created it after the initial existence check.
  if (::linkat(directory_, temporary_.c_str(), directory_, destination_.c_str(), 0) != 0) {
    return errno == EEXIST ? Status::EXISTS : Status::IO;
  }
  if (::unlinkat(directory_, temporary_.c_str(), 0) == 0) temporary_.clear();
  return Status::OK;
#endif
}

} // namespace file_sql
} // namespace sql
} // namespace oceanbase
