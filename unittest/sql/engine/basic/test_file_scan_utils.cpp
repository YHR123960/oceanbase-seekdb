/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

#include "sql/engine/basic/file_scan_utils.h"

namespace oceanbase::sql::file_sql
{
namespace
{
class FileScanTest : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char path[] = "/tmp/seekdb-file-scan-XXXXXX";
    ASSERT_NE(nullptr, ::mkdtemp(path));
    root_ = path;
  }

  void TearDown() override
  {
    for (const auto &path : files_) ::unlink(path.c_str());
    ::rmdir(root_.c_str());
  }

  std::string file(const char *name)
  {
    files_.push_back(root_ + "/" + name);
    return files_.back();
  }

  std::string root_;
  std::vector<std::string> files_;
};

TEST_F(FileScanTest, CsvSchemaAndRescan)
{
  ScanSpec spec;
  spec.path = file("data.csv");
  std::ofstream(spec.path) << "\xef\xbb\xbfid,name,id,day,ts,flag,n\r\n"
    "1,\"a,b\r\n\" \"\",10,2024-02-29,2026-01-01T00:00:00,true,\r\n";
  Reader reader;
  ASSERT_EQ(Status::INVALID, reader.infer(root_, spec));
  EXPECT_NE(std::string::npos, reader.error().find("line 2"));
  std::ofstream(spec.path) << "\xef\xbb\xbf" "id,name,id,day,ts,flag,n\r\n"
    "1,\"a,b\r\n\"\"quoted\"\"\",10,2024-02-29,2026-01-01T00:00:00,true,\r\n"
    "2,\"\xe9\x9b\xaa\",11,,2026-01-01 00:00:00.125,false,NULL\r\n";
  ASSERT_EQ(Status::OK, reader.infer(root_, spec)) << reader.error();
  EXPECT_EQ("id__2", spec.columns[2].name);
  EXPECT_EQ(Type::DATE, spec.columns[3].type);
  EXPECT_TRUE(spec.columns[3].nullable);
  EXPECT_EQ(Type::DATETIME, spec.columns[4].type);
  EXPECT_EQ(3, spec.columns[4].temporal_scale);
  EXPECT_EQ(Type::BOOLEAN, spec.columns[5].type);
  EXPECT_EQ(Type::NIL, spec.columns[6].type);
  ScanSpec decoded;
  const std::string encoded = spec.encode();
  EXPECT_FALSE(decoded.decode(encoded.data(), encoded.size() - 1));
  ASSERT_TRUE(decoded.decode(encoded.data(), encoded.size()));
  for (int pass = 0; pass < 2; ++pass) {
    ASSERT_EQ(Status::OK, reader.open(root_, decoded, {2, 1, 6}));
    std::vector<Cell> row;
    ASSERT_EQ(Status::OK, reader.next(row));
    ASSERT_EQ(3u, row.size());
    EXPECT_EQ("10", row[0].text);
    EXPECT_EQ("a,b\r\n\"quoted\"", row[1].text);
    EXPECT_EQ(Type::NIL, row[2].type);
    ASSERT_EQ(Status::OK, reader.next(row));
    EXPECT_EQ("\xe9\x9b\xaa", row[1].text);
    EXPECT_EQ(Status::END, reader.next(row));
  }
}

TEST_F(FileScanTest, JsonUnionAndConflicts)
{
  ScanSpec spec;
  spec.options.kind = Kind::JSONL;
  spec.path = file("data.jsonl");
  std::ofstream(spec.path) << R"({"id":-9223372036854775808,"v":2,"mixed":"2"})" << '\n'
    << R"({"v":2.5,"mixed":true,"late":"\uD83D\uDE00","date":"2024-02-29"})" << '\n'
    << R"({"v":null,"big":9223372036854775808})" << '\n';
  Reader reader;
  ASSERT_EQ(Status::OK, reader.infer(root_, spec)) << reader.error();
  EXPECT_EQ(Type::BIGINT, spec.columns[0].type);
  EXPECT_TRUE(spec.columns[0].nullable);
  EXPECT_EQ(Type::DOUBLE, spec.columns[1].type);
  EXPECT_EQ(Type::VARCHAR, spec.columns[2].type);
  EXPECT_EQ(Type::DATE, spec.columns[4].type);
  EXPECT_EQ(Type::VARCHAR, spec.columns[5].type);
  ASSERT_EQ(Status::OK, reader.open(root_, spec, {0, 3}));
  std::vector<Cell> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ("-9223372036854775808", row[0].text);
  EXPECT_EQ(Type::NIL, row[1].type);
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ(Type::NIL, row[0].type);
  EXPECT_EQ("\xf0\x9f\x98\x80", row[1].text);
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ(Status::END, reader.next(row));
  std::ofstream(spec.path) << "{}\n{\"nested\":[1]}\n";
  EXPECT_EQ(Status::INVALID, reader.infer(root_, spec));
  EXPECT_NE(std::string::npos, reader.error().find("line 2"));
}

TEST_F(FileScanTest, PathAndMutation)
{
  ScanSpec spec;
  Reader reader;
  spec.path = file("data.csv");
  std::ofstream(spec.path) << "id\n1\n";
  EXPECT_EQ(Status::DENIED, reader.infer(root_ + "-other", spec));
  ScanSpec unsafe;
  unsafe.path = root_ + "/../etc/passwd";
  EXPECT_EQ(Status::DENIED, reader.infer(root_, unsafe));
  unsafe.path = file("link.csv");
  ASSERT_EQ(0, ::symlink("/etc/passwd", unsafe.path.c_str()));
  EXPECT_EQ(Status::DENIED, reader.infer(root_, unsafe));
  ASSERT_EQ(Status::OK, reader.infer(root_, spec));
  ASSERT_EQ(Status::OK, reader.open(root_, spec, {0}));
  const std::string replacement = file("replacement");
  std::ofstream(replacement) << "id\n2\n";
  ASSERT_EQ(0, ::rename(replacement.c_str(), spec.path.c_str()));
  std::vector<Cell> row;
  EXPECT_EQ(Status::CHANGED, reader.next(row));
  EXPECT_EQ(Status::CHANGED, reader.open(root_, spec, {0}));
  spec.options.kind = Kind::GLOB;
  spec.path = root_ + "/*.csv";
  ASSERT_EQ(Status::OK, reader.infer(root_, spec));
  ASSERT_EQ(Status::OK, reader.open(root_, spec, {0}));
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ(root_ + "/data.csv", row[0].text);
  EXPECT_EQ(Status::END, reader.next(row));
}

TEST_F(FileScanTest, BoundedStreamingAndSchemaDescription)
{
  ScanSpec spec;
  spec.path = file("large.csv");
  {
    std::ofstream output(spec.path);
    output << "v\n";
    const std::string value(1024, 'a');
    for (int i = 0; i < 65536; ++i) output << value << '\n';
    output << "end\n";
  }
  Reader reader;
  ASSERT_EQ(Status::OK, reader.infer(root_, spec));
  EXPECT_EQ(Type::VARCHAR, spec.columns[0].type);
  ASSERT_EQ(Status::OK, reader.open(root_, spec, {}));
  std::vector<Cell> row;
  for (int i = 0; i < 65537; ++i) {
    ASSERT_EQ(Status::OK, reader.next(row));
    EXPECT_TRUE(row.empty());
  }
  EXPECT_EQ(Status::END, reader.next(row));
  spec.describe = true;
  ASSERT_EQ(Status::OK, reader.open(root_, spec, {0, 1, 2, 3}));
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ("v", row[0].text);
  EXPECT_EQ("VARCHAR", row[1].text);
  EXPECT_EQ("NO", row[2].text);
  EXPECT_EQ(Type::NIL, row[3].type);
  EXPECT_EQ(Status::END, reader.next(row));
}

TEST_F(FileScanTest, CopyRoundTripAndNoOverwrite)
{
  ScanSpec spec;
  spec.path = file("export.csv");
  spec.options.delimiter = '|';
  spec.options.escape = '\\';
  spec.options.null_text = "MISSING";
  {
    CsvWriter writer;
    ASSERT_EQ(Status::OK, writer.open(root_, spec.path, spec.options));
    EXPECT_EQ(Status::OK, writer.append({{Type::VARCHAR, "a"}, {Type::VARCHAR, "b"}}));
    EXPECT_EQ(Status::OK, writer.append({{Type::VARCHAR, "a|\n\\\"z"}, {}}));
    EXPECT_EQ(Status::OK, writer.append({{Type::VARCHAR, "MISSING"}, {Type::VARCHAR, ""}}));
    EXPECT_EQ(Status::OK, writer.publish());
  }
  CsvWriter overwrite;
  EXPECT_EQ(Status::EXISTS, overwrite.open(root_, spec.path, spec.options));
  Reader reader;
  ASSERT_EQ(Status::OK, reader.infer(root_, spec)) << reader.error();
  ASSERT_EQ(Status::OK, reader.open(root_, spec, {0, 1}));
  std::vector<Cell> row;
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ("a|\n\\\"z", row[0].text);
  EXPECT_EQ(Type::NIL, row[1].type);
  ASSERT_EQ(Status::OK, reader.next(row));
  EXPECT_EQ("MISSING", row[0].text);
  EXPECT_EQ(Type::VARCHAR, row[1].type);
  EXPECT_TRUE(row[1].text.empty());
  EXPECT_EQ(Status::END, reader.next(row));

  const std::string aborted = file("aborted.csv");
  {
    CsvWriter writer;
    ASSERT_EQ(Status::OK, writer.open(root_, aborted, spec.options));
    ASSERT_EQ(Status::OK, writer.append({{Type::VARCHAR, "partial"}}));
  }
  EXPECT_NE(0, ::access(aborted.c_str(), F_OK));
  DIR *dir = ::opendir(root_.c_str());
  ASSERT_NE(nullptr, dir);
  for (const dirent *entry = ::readdir(dir); entry != nullptr; entry = ::readdir(dir)) {
    EXPECT_NE(0, std::string(entry->d_name).find(".seekdb-copy-"));
  }
  ::closedir(dir);
}

#ifdef SEEKDB_FILE_SQL_ARROW
TEST_F(FileScanTest, ParquetProjectionAcrossGroups)
{
  ScanSpec spec;
  spec.options.kind = Kind::PARQUET;
  const std::string test_path(__FILE__);
  const std::string fixture = test_path.substr(0, test_path.find_last_of('/')) + "/data/multi_row_group.parquet";
  char fixture_path[PATH_MAX];
  ASSERT_NE(nullptr, ::realpath(fixture.c_str(), fixture_path));
  spec.path = file("groups.parquet");
  {
    std::ifstream src(fixture_path, std::ios::binary);
    std::ofstream dest(spec.path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(src) && static_cast<bool>(dest));
    dest << src.rdbuf();
  }

  Reader reader;
  ASSERT_EQ(Status::OK, reader.infer(root_, spec)) << reader.error();
  ASSERT_EQ(5u, spec.columns.size());
  EXPECT_EQ(Type::DOUBLE, spec.columns[2].type);
  EXPECT_EQ(Type::DATE, spec.columns[3].type);
  EXPECT_EQ(Type::DATETIME, spec.columns[4].type);
  EXPECT_FALSE(spec.columns[0].nullable);
  EXPECT_TRUE(spec.columns[1].nullable);
  for (const auto &projection : std::vector<std::vector<int64_t>>{{1, 0}, {0}, {}}) {
    ASSERT_EQ(Status::OK, reader.open(root_, spec, projection)) << reader.error();
    std::vector<Cell> row;
    for (int i = 0; i < 7; ++i) {
      ASSERT_EQ(Status::OK, reader.next(row)) << reader.error();
      ASSERT_EQ(projection.size(), row.size());
      if (!row.empty()) { EXPECT_EQ(std::to_string(i), row.back().text); }
      if (projection.size() == 2) {
        if (i == 4) EXPECT_EQ(Type::NIL, row[0].type);
        else EXPECT_EQ("row-" + std::to_string(i), row[0].text);
      }
    }
    EXPECT_EQ(Status::END, reader.next(row));
  }
  ASSERT_EQ(Status::OK, reader.open(root_, spec, {2, 3, 4}));
  std::vector<Cell> row;
  ASSERT_EQ(Status::OK, reader.next(row)) << reader.error();
  EXPECT_EQ("0.125", row[0].text);
  EXPECT_EQ("2024-02-29", row[1].text);
  int64_t timestamp = 0;
  EXPECT_TRUE(parse_temporal(row[2].text, timestamp));
}
#endif

} // namespace
} // namespace oceanbase::sql::file_sql
