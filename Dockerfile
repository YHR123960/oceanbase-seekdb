# MiniVectorDB — 轻量向量数据库
#
# 运行环境为 Linux x86_64 容器，构建阶段安装依赖并锁定版本，
# 运行阶段不联网。程序以前台方式运行，通过 stdin/stdout 与评测
# 驱动按 JSONL 协议通信，数据目录为 /data。

FROM python:3.12-slim-bookworm AS build

ENV PIP_NO_CACHE_DIR=1 \
    PIP_DISABLE_PIP_VERSION_CHECK=1
RUN pip install --index-url https://mirrors.aliyun.com/pypi/simple/ numpy==2.2.6 \
    || pip install numpy==2.2.6

FROM python:3.12-slim-bookworm

COPY --from=build /usr/local/lib/python3.12/site-packages /usr/local/lib/python3.12/site-packages
WORKDIR /app
COPY minivectordb/ /app/minivectordb/

RUN mkdir -p /data && chmod -R a+rX /app
VOLUME ["/data"]

ENTRYPOINT ["python", "-u", "/app/minivectordb/main.py", "--data-dir", "/data"]
