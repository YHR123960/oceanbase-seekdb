/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace oceanbase::sql
{
struct ObDMLBaseCtDef;
struct ObUpdCtDef;
class ObTableModifyOp;
class ObExecContext;

bool is_ai_column_task_nested_sql(const ObExecContext *parent);

// Enlist the queue mutation using the business statement's existing transaction.
int maintain_ai_column_tasks(ObTableModifyOp &op, const ObDMLBaseCtDef &dml,
                             bool has_old_row, bool has_new_row,
                             const ObUpdCtDef *update = nullptr);
}
