# MiniVectorDB — 支持 SQL、持久化与事务的轻量向量数据库

PowerContext 2026 赛题（seekdb `2026-competition-round2`）参赛实现。

## 运行

程序以 JSON Lines 协议通过 stdin/stdout 与评测驱动通信（协议见题包 STDIO_PROTOCOL.md）：

```bash
docker build -t minivectordb .
docker run -i --rm minivectordb --data-dir /data
```

也可直接运行：

```bash
pip install numpy==2.2.6
python -u minivectordb/main.py --data-dir /data
```

## 结构

- `minivectordb/` — 数据库实现（SQL 解析、执行引擎、追加式持久化存储、IVF 向量索引、事务与崩溃恢复、JSONL 协议服务）
- `Dockerfile` — python:3.12-slim，锁定 numpy==2.2.6，前台 ENTRYPOINT，数据目录 /data
- `agent/conversation.json` — Agent prompt 学习材料（实际提示词、上下文、实验证据）
