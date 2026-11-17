"""Persistent storage: append-only per-table record files.

Design notes
------------
Each table is one append-only file ``tables/<name>.dat`` that doubles as its
own write-ahead log.  A record is ``op(1) | length(u32 LE) | payload``:

- op 1 (upsert): payload = pk(i64) + every non-pk column value in schema order
- op 2 (delete): payload = pk(i64)

Only committed data reaches the file: the engine validates a statement or
transaction completely, appends all records with a single ``os.write`` and
``fsync`` before acknowledging.  A crash therefore leaves either all or none
of a statement/transaction in the file, and startup recovery replays the file
to rebuild the primary-key index without any separate checkpoint.

Value encodings: BIGINT as i64, VARCHAR as u32 length + UTF-8 bytes, VECTOR
as dim float32 little-endian.  Vector values travel through the engine as
raw little-endian float32 ``bytes``.
"""

import json
import os
import struct

from errors import DatabaseError, syntax_error

_BINARY = getattr(os, "O_BINARY", 0)  # Windows: avoid CRT text-mode translation

OP_UPSERT = 1
OP_DELETE = 2
_HEADER = struct.Struct("<BI")
_I64 = struct.Struct("<q")
_U32 = struct.Struct("<I")

TABLES_SUBDIR = "tables"
SCHEMA_FILE = "schema.json"

if hasattr(os, "pread"):
    def _pread(fd, length, offset):
        return os.pread(fd, length, offset)
else:
    def _pread(fd, length, offset):
        # Windows fallback: single-threaded, so seek+read is safe.
        os.lseek(fd, offset, os.SEEK_SET)
        data = bytearray()
        while len(data) < length:
            chunk = os.read(fd, length - len(data))
            if not chunk:
                break
            data += chunk
        return bytes(data)

# Cache budget: tables whose file is at most this size may keep parsed rows
# and a vector matrix in memory; larger tables are always streamed from disk.
ROW_CACHE_LIMIT = 96 * 1024 * 1024


def _fsync_dir(path):
    try:
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    except OSError:
        # Directory fsync is not supported everywhere (e.g. Windows).
        pass


def encode_value(column, value):
    if column["type"] == "BIGINT":
        return _I64.pack(value)
    if column["type"] == "VARCHAR":
        data = value.encode("utf-8")
        return _U32.pack(len(data)) + data
    return value  # VECTOR: already little-endian float32 bytes


def decode_values(columns, payload, pos):
    row = {}
    for column in columns:
        if column["type"] == "BIGINT":
            (value,) = _I64.unpack_from(payload, pos)
            pos += 8
        elif column["type"] == "VARCHAR":
            (length,) = _U32.unpack_from(payload, pos)
            pos += 4
            value = payload[pos:pos + length].decode("utf-8")
            pos += length
        else:
            dim = column["size"]
            value = payload[pos:pos + 4 * dim]
            pos += 4 * dim
        row[column["name"]] = value
    return row, pos


class Schema:
    """Table definitions, persisted atomically in schema.json."""

    def __init__(self, data_dir):
        self.data_dir = data_dir
        self.tables = {}  # name -> {"columns": [...], "vector_index": {...}|None}
        self.path = os.path.join(data_dir, SCHEMA_FILE)
        self._load()

    def _load(self):
        if not os.path.exists(self.path):
            return
        with open(self.path, "rb") as handle:
            document = json.loads(handle.read().decode("utf-8"))
        for name, definition in document.get("tables", {}).items():
            self.tables[name] = {
                "columns": definition["columns"],
                "vector_index": definition.get("vector_index"),
            }

    def flush(self):
        document = {"version": 1, "tables": self.tables}
        tmp = self.path + ".tmp"
        with open(tmp, "wb") as handle:
            handle.write(json.dumps(document, ensure_ascii=False).encode("utf-8"))
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, self.path)
        _fsync_dir(self.data_dir)

    def create_table(self, name, columns):
        if name in self.tables:
            raise DatabaseError("table_already_exists", f"table {name} already exists")
        self.tables[name] = {"columns": columns, "vector_index": None}
        self.flush()

    def set_vector_index(self, name, index_name, column):
        self.tables[name]["vector_index"] = {"name": index_name, "column": column}
        self.flush()


class TableFile:
    """Append-only record file plus the in-memory primary-key index."""

    def __init__(self, schema_dir, name, columns):
        self.name = name
        self.columns = columns
        self.pk_column = next(c for c in columns if c.get("primary_key"))
        self.value_columns = [c for c in columns if not c.get("primary_key")]
        directory = os.path.join(schema_dir, TABLES_SUBDIR)
        os.makedirs(directory, exist_ok=True)
        self.path = os.path.join(directory, name + ".dat")
        self.index = {}  # pk -> (offset, length) of the latest upsert record
        self.row_count = 0
        self.dead_count = 0
        self.file_size = 0
        self._write_fd = None
        self._read_fd = None
        self._vector_index_dirty = False

    # -- lifecycle ----------------------------------------------------------

    def open(self):
        self._write_fd = os.open(self.path, os.O_RDWR | os.O_CREAT | os.O_APPEND | _BINARY)
        self._read_fd = os.open(self.path, os.O_RDONLY | _BINARY)
        self._recover()
        self.file_size = os.fstat(self._write_fd).st_size

    def close(self):
        for fd in (self._write_fd, self._read_fd):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        self._write_fd = self._read_fd = None

    # -- recovery -----------------------------------------------------------

    def _recover(self):
        """Rebuild the primary-key index by scanning the whole file."""
        size = os.fstat(self._read_fd).st_size
        self.file_size = size
        index = {}
        dead = 0
        rows = 0
        if size == 0:
            self.index = index
            return
        with open(self.path, "rb") as handle:
            data = handle.read()
        pos = 0
        good = 0  # byte offset after the last fully-valid record
        while pos < size:
            start = pos
            if pos + 5 > size:
                break  # torn header tail
            op, length = _HEADER.unpack_from(data, pos)
            pos += 5 + length
            if pos > size or op not in (OP_UPSERT, OP_DELETE):
                pos = start
                break  # torn or corrupt record
            if op == OP_UPSERT:
                (pk,) = _I64.unpack_from(data, start + 5)
                if index.pop(pk, None) is not None:
                    dead += 1
                else:
                    rows += 1
                index[pk] = (start + 5, length)
            else:
                (pk,) = _I64.unpack_from(data, start + 5)
                if index.pop(pk, None) is not None:
                    rows -= 1
                dead += 1
            good = pos
        self.index = index
        self.row_count = rows
        self.dead_count = dead
        # A crash may leave a torn tail; drop it so appends stay parseable.
        if good < size:
            self._truncate(good)

    def _truncate(self, size):
        os.ftruncate(self._write_fd, size)
        os.fsync(self._write_fd)
        self.file_size = size

    # -- committed writes ---------------------------------------------------

    def append_records(self, entries):
        """Append fully-validated records atomically, durably, then index them.

        ``entries`` is an ordered list of ``(record_bytes, op, pk)`` covering
        every mutation of one statement or transaction.  A single ``os.write``
        makes the group atomic even under a crash mid-append, and the file is
        fsynced before the caller acknowledges success.
        """
        blob = b"".join(record for record, _, _ in entries)
        prior_size = self.file_size
        try:
            os.write(self._write_fd, blob)
            os.fsync(self._write_fd)
        except OSError:
            # Roll the file back to the last durable boundary.
            os.ftruncate(self._write_fd, prior_size)
            raise
        offset = prior_size
        for record, op, pk in entries:
            if op == OP_UPSERT:
                if self.index.get(pk) is not None:
                    self.dead_count += 1
                else:
                    self.row_count += 1
                self.index[pk] = (offset + 5, len(record) - 5)
            else:
                if self.index.pop(pk, None) is not None:
                    self.row_count -= 1
                self.dead_count += 1
            offset += len(record)
        self.file_size = offset

    def encode_upsert(self, row):
        payload = bytearray(_I64.pack(row[self.pk_column["name"]]))
        for column in self.value_columns:
            payload += encode_value(column, row[column["name"]])
        return _HEADER.pack(OP_UPSERT, len(payload)) + bytes(payload)

    def encode_delete(self, pk):
        payload = _I64.pack(pk)
        return _HEADER.pack(OP_DELETE, len(payload)) + payload

    # -- reads --------------------------------------------------------------

    def read_row(self, pk):
        entry = self.index.get(pk)
        if entry is None:
            return None
        offset, length = entry
        payload = _pread(self._read_fd, length, offset)
        row, _ = decode_values(self.value_columns, payload, 8)
        row[self.pk_column["name"]] = pk
        return row

    def scan_latest(self):
        """Yield (pk, row) for the latest version of every live row.

        Streams the file sequentially; memory stays bounded by one record.
        """
        offset = 0
        size = self.file_size
        index = self.index
        with open(self.path, "rb") as handle:
            # Buffered sequential read is much faster than repeated pread.
            while offset < size:
                header = handle.read(5)
                if len(header) < 5:
                    return
                op, length = _HEADER.unpack(header)
                payload = handle.read(length)
                if len(payload) < length:
                    return
                if op == OP_UPSERT:
                    (pk,) = _I64.unpack_from(payload, 0)
                    entry = index.get(pk)
                    if entry is not None and entry[0] == offset + 5:
                        row, _ = decode_values(self.value_columns, payload, 8)
                        row[self.pk_column["name"]] = pk
                        yield pk, row
                offset += 5 + length

    # -- compaction ---------------------------------------------------------

    def needs_compaction(self):
        return self.dead_count > 0 and (
            self.dead_count >= self.row_count
            or (self.dead_count > 2 * self.row_count and self.file_size > 16 * 1024 * 1024)
        )

    def compact(self):
        """Rewrite the file with only live rows; atomic via tmp + replace."""
        tmp = self.path + ".tmp"
        index = {}
        rows = 0
        offset = 0
        with open(tmp, "wb") as out:
            for pk, row in self.scan_latest():
                record = self.encode_upsert(row)
                out.write(record)
                index[pk] = (offset + 5, len(record) - 5)
                offset += len(record)
                rows += 1
            out.flush()
            os.fsync(out.fileno())
        # Close the handles on the old file before replacing it: platforms
        # such as Windows refuse to replace a file that is still open.
        old_read, old_write = self._read_fd, self._write_fd
        self._read_fd = self._write_fd = None
        for fd in (old_read, old_write):
            os.close(fd)
        try:
            os.replace(tmp, self.path)
        except OSError:
            # Restore the handles on the original file; state is unchanged.
            self._read_fd = os.open(self.path, os.O_RDONLY)
            self._write_fd = os.open(self.path, os.O_RDWR | os.O_APPEND)
            raise
        _fsync_dir(os.path.dirname(self.path))
        self._read_fd = os.open(self.path, os.O_RDONLY | _BINARY)
        self._write_fd = os.open(self.path, os.O_RDWR | os.O_APPEND | _BINARY)
        self.index = index
        self.row_count = rows
        self.dead_count = 0
        self.file_size = offset
