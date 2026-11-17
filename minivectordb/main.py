"""MiniVectorDB JSONL protocol entry point.

Startup: open /data, replay recovery, then emit {"type":"ready"}.
Loop: one JSON request per stdin line, one JSON response per stdout line.
Shutdown: {"op":"shutdown"} rolls back any open transaction, fsyncs, answers
ok and exits 0 without writing anything more to stdout.
"""

import argparse
import json
import os
import sys

from engine import Engine
from errors import DatabaseError
from protocol_strict import strict_json


def emit(message):
    sys.stdout.write(json.dumps(message, ensure_ascii=False, allow_nan=False) + "\n")
    sys.stdout.flush()


def handle(engine, request):
    ident = request["id"]
    op = request.get("op")
    if op == "shutdown":
        engine.shutdown()
        emit({"id": ident, "ok": True})
        return True
    if op != "execute":
        raise DatabaseError("unsupported_op", f"unsupported operation {op!r}")
    sql = request["sql"]
    mode = request.get("search_mode", "exact")
    if mode not in ("exact", "ann"):
        raise DatabaseError("unsupported_op", f"unsupported search_mode {mode!r}")
    payload = engine.execute(sql, mode)
    response = {"id": ident, "ok": True}
    response.update(payload)
    emit(response)
    return False


def serve(engine):
    stdin = sys.stdin
    while True:
        line = stdin.readline()
        if not line:
            break  # EOF: the judge closed stdin
        if not line.strip():
            continue
        try:
            request = strict_json(line)
        except ValueError as exc:
            sys.stderr.write(f"invalid request JSON: {exc}\n")
            continue
        try:
            if handle(engine, request):
                return 0
        except DatabaseError as exc:
            emit({
                "id": request["id"],
                "ok": False,
                "error": {"code": exc.code, "message": str(exc)},
            })
        except Exception as exc:  # never kill the process on unexpected errors
            sys.stderr.write(f"internal error: {exc!r}\n")
            emit({
                "id": request["id"],
                "ok": False,
                "error": {"code": "internal_error", "message": repr(exc)},
            })
    engine.shutdown()
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description="MiniVectorDB (JSONL stdio)")
    parser.add_argument("--data-dir", default=os.environ.get("VECTORDB_DATA_DIR", "/data"))
    args = parser.parse_args(argv)
    engine = Engine(args.data_dir)
    engine.finish_startup()
    emit({"type": "ready"})
    return serve(engine)


if __name__ == "__main__":
    sys.exit(main())
