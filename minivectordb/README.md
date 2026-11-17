# MiniVectorDB —轻量向量数据库（Python + NumPy 实现）

从零实现的单机向量数据库，支持 SQL 子集、持久化存储、事务与崩溃恢复、
精确与近似向量检索。遵循 `2026-competition-round2` 题包的 JSONL stdio
协议（见 `STDIO_PROTOCOL.md`），MySQL case 自测驱动见 `protocol_check/`。

## 语言与依赖

- Python 3.12（标准库 + NumPy）
- NumPy 2.2.6（构建阶段安装并锁定，运行阶段不联网）

## 镜像构建与运行

```bash
docker build -t minivectordb:latest .
docker run --rm -i \
  --mount "type=volume,src=minivectordb-data,dst=/data" \
  minivectordb:latest
```

容器以前台方式运行数据库进程，通过 stdin/stdout 与测试驱动通信，
不监听任何端口。数据文件统一写入 `/data`。

## 启动入口

`ENTRYPOINT` 固定为：

```
python /app/main.py --data-dir /data
```

数据目录可通过 `--data-dir` 参数或 `VECTORDB_DATA_DIR` 环境变量覆盖，
评测环境默认 `/data`。

## SQL 支持范围

| 语句 | 说明 |
| --- | --- |
| `CREATE TABLE` | `BIGINT`、`VARCHAR(n)`、`VECTOR(n)`；恰好一个 `BIGINT PRIMARY KEY`，每表最多一个向量列 |
| `CREATE VECTOR INDEX name ON t(col)` | 自实现 IVF（k-means 聚类 + 倒排桶 + 精确重排）；小表自动退化为精确扫描 |
| `INSERT` | 单行 / 多行；支持显式列清单 |
| `SELECT` | 列投影 / `*`；`WHERE`（`=`,`<`,`<=`,`>`,`>=` 与 `AND`、括号）；`ORDER BY`（列 / 别名 / 序数 / `L2_DISTANCE` 表达式，支持 `DESC`）；`LIMIT`；`L2_DISTANCE(col, '[...]')` 投影 |
| `UPDATE` / `DELETE` | 按主键或条件；不支持主键更新 |
| `BEGIN` / `COMMIT` / `ROLLBACK` | 单事务；事务内读己之写；语句失败不留部分修改 |
| 向量检索 | 精确 TopK（`search_mode=exact`）、近似 TopK（`ann`）；过滤在 TopK 前生效；距离并列按主键升序 |

## 错误处理

预期中的 SQL 错误返回 `{"ok": false, "error": {"code", "message"}}` 后继续
服务后续请求，进程不退出。错误码为语义名（snake_case），公开用例已确认
`duplicate_key`、`unsupported_sql`、`dimension_mismatch`；其余如
`syntax_error`、`table_not_found`、`column_not_found`、`type_mismatch`、
`value_out_of_range`、`invalid_vector` 等按同一风格命名，正式映射以题包
冻结配置为准。

## 从空数据目录开始的操作示例

驱动请求（JSONL，每行一个）：

```json
{"id":1,"op":"execute","sql":"CREATE TABLE docs (id BIGINT PRIMARY KEY, content VARCHAR(16), embedding VECTOR(4));","search_mode":"exact"}
{"id":2,"op":"execute","sql":"INSERT INTO docs VALUES (1, '事务与恢复', '[1,0,0,0]'), (2, '向量索引', '[0,1,0,0]');","search_mode":"exact"}
{"id":3,"op":"execute","sql":"SELECT id, L2_DISTANCE(embedding, '[0.9,0.1,0,0]') AS distance FROM docs ORDER BY distance LIMIT 2;","search_mode":"exact"}
{"id":4,"op":"execute","sql":"BEGIN;","search_mode":"exact"}
{"id":5,"op":"execute","sql":"UPDATE docs SET embedding = '[0.8,0.2,0,0]' WHERE id = 1;","search_mode":"exact"}
{"id":6,"op":"execute","sql":"ROLLBACK;","search_mode":"exact"}
{"id":7,"op":"shutdown"}
```

程序先完成恢复再输出 `{"type":"ready"}`；每条请求回一行含相同 `id` 的
响应；`COMMIT`/自动提交语句在 fsync 落盘后才返回成功。

## 本地自测

```bash
python protocol_check/run.py --image minivectordb:latest --handshake-only --output out/handshake
python protocol_check/run.py --image minivectordb:latest --output out/public
```

## 存储与恢复设计（简要）

- 每张表一个追加式记录文件 `tables/<name>.dat`，本身即写前日志：
  记录 = `op(1B) | payload_len(u32) | payload`，upsert 写整行，delete 写主键。
- 语句/事务先完整校验，再以**一次** `os.write` 原子追加并 `fsync`，
  成功响应前持久化完成；进程被强杀后重启扫描文件重建主键索引，
  已提交数据不丢，未提交事务不生效，重复恢复幂等。
- 大于内存预算的表走流式扫描（逐记录解码、谓词下推、TopK 用有界堆），
  不依赖全量常驻内存；小表额外维护解析行缓存与向量矩阵加速查询。
- 死元组比例过高时用「写临时文件 + 原子替换」压实，恢复启动阶段同样处理。

## 已知限制

- 不支持 NULL、JOIN、聚合、子查询、ALTER/DROP、嵌套事务、主键更新，
  按题目要求返回错误而不是静默执行。
- 未发布错误码映射按语义命名，正式题包冻结后如需对齐再调整。
- 近似索引随数据写入标记脏、下次 ann 查询时懒重建（重建前仍可提供
  精确模式与正确的事务语义），未做增量桶维护。
