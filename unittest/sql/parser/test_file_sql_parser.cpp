/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>
#include <cstring>

#include "lib/allocator/ob_allocator.h"
#include "lib/charset/ob_charset.h"
#include "sql/parser/parse_node.h"

// The standalone parser library has a PL callback. These SQL tests have no
// PL namespace, so invoking it would be a regression.
extern "C" int lookup_pl_symbol(const void *, const char *, size_t, int64_t *)
{
  ADD_FAILURE() << "unexpected PL lookup while parsing file SQL";
  return -1;
}

namespace oceanbase::sql
{
namespace
{
class FileSqlParserTest : public ::testing::Test
{
protected:
  const ParseNode *parse(const char *sql)
  {
    // Save the arena for inspection of the parse tree for the rest of this test.
    arena_.reuse();
    result_ = {};
    result_.malloc_pool_ = &arena_;
    result_.charset_info_ = common::ObCharset::get_charset(common::CS_TYPE_UTF8MB4_BIN);
    int ret = parse_init(&result_);
    if (ret == 0) {
      ret = parse_sql(&result_, sql, std::strlen(sql));
      parse_terminate(&result_);
    }
    if (ret != 0 || result_.result_tree_ == nullptr) {
      return nullptr;
    }
    return result_.result_tree_->children_[0];
  }

  common::ObArenaAllocator arena_{"FileSqlParser"};
  ParseResult result_{};
};

TEST_F(FileSqlParserTest, FileTableAliasAndNamedLiterals)
{
  const ParseNode *query = parse("SELECT f.id FROM read_csv('/data/f.csv', header = true, sample_size = -1) AS f");
  ASSERT_NE(nullptr, query);
  ASSERT_EQ(T_SELECT, query->type_);
  const ParseNode *table = query->children_[PARSE_SELECT_FROM]->children_[0];
  ASSERT_EQ(T_FILE_TABLE, table->type_);
  EXPECT_STREQ("read_csv", table->children_[0]->str_value_);
  EXPECT_STREQ("/data/f.csv", table->children_[1]->str_value_);
  ASSERT_NE(nullptr, table->children_[2]);
  ASSERT_EQ(2, table->children_[2]->num_child_);
  for (int i = 0; i < 2; ++i) {
    const ParseNode *pair = table->children_[2]->children_[i]->children_[0];
    ASSERT_NE(nullptr, pair);
    ASSERT_EQ(2, pair->num_child_);
  }
  const ParseNode *sample = table->children_[2]->children_[1]->children_[0]->children_[1];
  EXPECT_EQ(T_INT, sample->type_);
  EXPECT_EQ(-1, sample->value_);
  EXPECT_STREQ("f", table->children_[3]->str_value_);
}

TEST_F(FileSqlParserTest, DescribeFileAndOrdinaryExplain)
{
  const ParseNode *desc = parse("DESCRIBE SELECT * FROM read_ndjson('/data/f.jsonl')");
  ASSERT_NE(nullptr, desc);
  ASSERT_EQ(T_SELECT, desc->type_);
  const ParseNode *table = desc->children_[PARSE_SELECT_FROM]->children_[0];
  ASSERT_EQ(T_FILE_TABLE, table->type_);
  EXPECT_EQ(1, table->value_);
  EXPECT_TRUE(table->children_[2] == nullptr || table->children_[2]->num_child_ == 0);
  const ParseNode *explain = parse("EXPLAIN SELECT * FROM read_csv('/data/f.csv')");
  ASSERT_NE(nullptr, explain);
  EXPECT_EQ(T_EXPLAIN, explain->type_);
  const ParseNode *ordinary = parse("DESC regular_table");
  ASSERT_NE(nullptr, ordinary);
  EXPECT_EQ(T_SHOW_COLUMNS, ordinary->type_);
}

TEST_F(FileSqlParserTest, CopyTargetsAndOptions)
{
  for (const char *sql : {"COPY t TO '/data/f.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL')",
                          "COPY (SELECT id FROM read_csv('/data/in.csv') ORDER BY id) TO '/data/f.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL')"}) {
    const ParseNode *query = parse(sql);
    ASSERT_NE(nullptr, query) << sql;
    ASSERT_EQ(T_SELECT, query->type_);
    const ParseNode *into = query->children_[PARSE_SELECT_INTO_EXTRA];
    ASSERT_NE(nullptr, into);
    EXPECT_EQ(T_INTO_OUTFILE, into->type_);
    EXPECT_EQ(1, into->value_);
    EXPECT_STREQ("/data/f.csv", into->children_[0]->str_value_);
    ASSERT_NE(nullptr, into->children_[1]);
    EXPECT_EQ(3, into->children_[1]->num_child_);
  }
  EXPECT_EQ(nullptr, parse("SELECT * FROM read_csv('/data/f.csv', 'xml')"));
}

} // namespace
} // namespace oceanbase::sql
