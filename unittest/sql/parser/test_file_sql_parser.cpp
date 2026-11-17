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
  EXPECT_EQ(4, table->num_child_);
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
  ASSERT_EQ(5, table->num_child_);
  EXPECT_NE(nullptr, table->children_[4]);
  EXPECT_TRUE(table->children_[2] == nullptr || table->children_[2]->num_child_ == 0);
  const ParseNode *grouped = parse("SELECT id FROM (read_csv('/data/f.csv'))");
  ASSERT_NE(nullptr, grouped);
  const ParseNode *grouped_table = grouped->children_[PARSE_SELECT_FROM]->children_[0];
  ASSERT_EQ(T_FILE_TABLE, grouped_table->type_);
  EXPECT_EQ(4, grouped_table->num_child_);
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

TEST_F(FileSqlParserTest, AiColumnDefinitionPreservesExpressionAndOptions)
{
  const ParseNode *stmt = parse(
      "CREATE TABLE t (id BIGINT PRIMARY KEY, text VARCHAR(200), "
      "tags JSON AI COLUMN (AI_COMPLETE('model', concat('tag: ', text))) "
      "WITH (sync_mode='async', max_retries=2))");
  ASSERT_NE(nullptr, stmt);
  ASSERT_EQ(T_CREATE_TABLE, stmt->type_);
  const ParseNode *elements = stmt->children_[3];
  ASSERT_NE(nullptr, elements);
  ASSERT_EQ(3, elements->num_child_);
  const ParseNode *column = elements->children_[2];
  ASSERT_EQ(T_COLUMN_DEFINITION, column->type_);
  ASSERT_EQ(5, column->num_child_);
  const ParseNode *ai = column->children_[4];
  ASSERT_NE(nullptr, ai);
  ASSERT_EQ(T_AI_COLUMN_DEFINITION, ai->type_);
  ASSERT_EQ(T_FUN_SYS, ai->children_[0]->type_);
  EXPECT_STREQ("AI_COMPLETE('model', concat('tag: ', text))", ai->children_[0]->str_value_);
  ASSERT_NE(nullptr, ai->children_[1]);
  ASSERT_EQ(2, ai->children_[1]->num_child_);
  const ParseNode *retries = ai->children_[1]->children_[1];
  EXPECT_STREQ("max_retries", retries->children_[0]->str_value_);
  EXPECT_EQ(2, retries->children_[1]->value_);
}

TEST_F(FileSqlParserTest, AiColumnAlterAndOrdinaryAiIdentifier)
{
  const ParseNode *stmt = parse("ALTER TABLE t ADD COLUMN e VECTOR(4) AI COLUMN (AI_EMBED('m', body))");
  ASSERT_NE(nullptr, stmt);
  EXPECT_EQ(T_ALTER_TABLE, stmt->type_);
  const ParseNode *show = parse("SHOW AI COLUMNS FROM app.t LIKE 'e'");
  ASSERT_NE(nullptr, show);
  ASSERT_EQ(T_SHOW_COLUMNS, show->type_);
  EXPECT_EQ(4, show->children_[0]->value_);
  const ParseNode *ordinary = parse("CREATE TABLE t (ai INT, body VARCHAR(20))");
  ASSERT_NE(nullptr, ordinary);
  EXPECT_EQ(4, ordinary->children_[3]->children_[0]->num_child_);
  EXPECT_EQ(nullptr, parse("CREATE TABLE t (value INT AI COLUMN (AI_COMPLETE('m', 'text')) WITH ())"));
}

} // namespace
} // namespace oceanbase::sql
