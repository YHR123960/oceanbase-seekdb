/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>

#include "share/schema/ob_column_schema.h"

namespace oceanbase::share::schema
{
using namespace common;

TEST(AiColumnSchemaTest, CopiesAndSerializesDefinitionAndDependencies)
{
  ObArenaAllocator allocator("AiColumnTest");
  ObColumnSchemaV2 column(&allocator);
  column.set_column_id(17);
  ASSERT_EQ(OB_SUCCESS, column.set_column_name("embedding"));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_definition(ObString("AI_EMBED('model', body, 4)"), 3));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_source(ObString("AI_EMBED('model', body)")));
  ASSERT_EQ(OB_SUCCESS, column.add_type_info(ObString("VECTOR(4)")));
  ASSERT_EQ(OB_SUCCESS, column.add_cascaded_column_id(16));

  ObColumnSchemaV2 copy(&allocator);
  ASSERT_EQ(OB_SUCCESS, copy.assign(column));
  EXPECT_TRUE(copy.is_ai_column());
  EXPECT_EQ(column.get_ai_column_expression(), copy.get_ai_column_expression());
  EXPECT_EQ(column.get_ai_column_source(), copy.get_ai_column_source());
  EXPECT_EQ(3, copy.get_ai_column_max_retries());
  EXPECT_TRUE(copy.has_cascaded_column_id(16));

  char buffer[4096];
  int64_t end = 0;
  ASSERT_EQ(OB_SUCCESS, copy.serialize(buffer, sizeof(buffer), end));
  EXPECT_EQ(copy.get_serialize_size(), end);
  ObColumnSchemaV2 decoded(&allocator);
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, decoded.deserialize(buffer, end, pos));
  EXPECT_EQ(end, pos);
  EXPECT_TRUE(decoded.is_ai_column());
  EXPECT_EQ(column.get_ai_column_expression(), decoded.get_ai_column_expression());
  EXPECT_EQ(column.get_ai_column_source(), decoded.get_ai_column_source());
  EXPECT_EQ(3, decoded.get_ai_column_max_retries());
  EXPECT_TRUE(decoded.has_cascaded_column_id(16));
  ASSERT_EQ(1, decoded.get_extended_type_info().count());
  EXPECT_EQ(ObString("VECTOR(4)"), decoded.get_extended_type_info().at(0));

  hash::ObHashMap<uint64_t, uint64_t> ids;
  ASSERT_EQ(OB_SUCCESS, ids.create(16, ObMemAttr("AiColumnTest")));
  ASSERT_EQ(OB_SUCCESS, ids.set_refactored(16, 22));
  ASSERT_EQ(OB_SUCCESS, ids.set_refactored(17, 23));
  ASSERT_EQ(OB_SUCCESS, decoded.convert_column_id(ids));
  EXPECT_EQ(23U, decoded.get_column_id());
  EXPECT_FALSE(decoded.has_cascaded_column_id(16));
  EXPECT_TRUE(decoded.has_cascaded_column_id(22));
  EXPECT_EQ(column.get_ai_column_expression(), decoded.get_ai_column_expression());
}

TEST(AiColumnSchemaTest, StoresSourceAndDependenciesAlongsideCollectionType)
{
  ObArenaAllocator allocator("AiColumnTest");
  ObColumnSchemaV2 column(&allocator);
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_definition(ObString("AI_EMBED('model', body, 4)"), 2));
  ASSERT_EQ(OB_SUCCESS, column.set_ai_column_source(ObString("AI_EMBED('model', body)")));
  ASSERT_EQ(OB_SUCCESS, column.add_type_info(ObString("VECTOR(4)")));
  ASSERT_EQ(OB_SUCCESS, column.add_cascaded_column_id(16));

  char buffer[1024];
  int64_t end = 0;
  ASSERT_EQ(OB_SUCCESS, column.serialize_extended_type_info(buffer, sizeof(buffer), end));

  ObColumnSchemaV2 restored(&allocator);
  restored.set_column_flags(column.get_stored_column_flags());
  int64_t pos = 0;
  ASSERT_EQ(OB_SUCCESS, restored.deserialize_extended_type_info(buffer, end, pos));
  EXPECT_EQ(end, pos);
  ASSERT_EQ(1, restored.get_extended_type_info().count());
  EXPECT_EQ(ObString("VECTOR(4)"), restored.get_extended_type_info().at(0));
  EXPECT_EQ(column.get_ai_column_source(), restored.get_ai_column_source());
  EXPECT_TRUE(restored.has_cascaded_column_id(16));
}

} // namespace oceanbase::share::schema
