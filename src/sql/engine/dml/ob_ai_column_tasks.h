/*
 * Copyright (c) 2026 OceanBase.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

namespace oceanbase::common { class ObSqlString; class ObString; }

namespace oceanbase::sql
{
struct ObDMLBaseCtDef;
struct ObUpdCtDef;
class ObTableModifyOp;
class ObExecContext;

extern thread_local bool ai_column_worker_write;
int get_ai_column_state_name(const common::ObString &column, common::ObSqlString &name);

bool is_ai_column_task_nested_sql(const ObExecContext *parent);

// Enlist the queue mutation using the business statement's existing transaction.
int maintain_ai_column_tasks(ObTableModifyOp &op, const ObDMLBaseCtDef &dml,
                             bool has_old_row, bool has_new_row,
                             const ObUpdCtDef *update = nullptr);
}
