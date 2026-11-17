"""SQL execution engine: tables, transactions, vector retrieval.

Execution model
---------------
- Committed state lives in append-only table files (see storage.py).  Every
  auto-commit statement is fully validated, then appended and fsynced before
  the success response, which gives statement atomicity and durability.
- An explicit transaction buffers its encoded records and keeps an in-memory
  overlay (pk -> row or DELETED) so reads see their own writes.  COMMIT
  appends the whole buffer once and fsyncs; ROLLBACK discards it.  A crash
  therefore never partially applies a transaction.
- Scans stream records from disk and keep only the latest version per primary
  key, so tables larger than the memory budget are handled without full
  residency.  Small tables additionally keep parsed-row and vector caches as
  an accelerator; both are rebuilt lazily and invalidated on any write.
"""

import heapq
import os

import numpy as np

import sqlparser
from ann import IVFIndex, EXACT_SCAN_THRESHOLD
from errors import DatabaseError, syntax_error, unsupported_sql
from storage import OP_UPSERT, OP_DELETE, Schema, TableFile, ROW_CACHE_LIMIT
from vectors import (
    chunk_l2_distances,
    l2_distance,
    parse_vector_text,
)

I64_MIN = -(2 ** 63)
I64_MAX = 2 ** 63 - 1

DELETED = object()  # overlay tombstone

_SORT_CHUNK = 200000  # rows before spilling sorted output to disk


class Transaction:
    def __init__(self):
        self.overlay = {}  # table -> {pk: row | DELETED}
        self.entries = {}  # table -> [(record, op, pk), ...]
        self.touched = set()


class Engine:
    def __init__(self, data_dir):
        os.makedirs(data_dir, exist_ok=True)
        self.data_dir = data_dir
        self.schema = Schema(data_dir)
        self.tables = {}
        for name, definition in self.schema.tables.items():
            table = TableFile(data_dir, name, definition["columns"])
            table.open()
            self.tables[name] = table
        self.txn = None
        # Lazily built accelerators, all invalidated on committed writes.
        self._row_cache = {}       # table -> dict pk -> row
        self._vector_cache = {}    # table -> (pks list, pk->pos, matrix) or None
        self._ann = {}             # table -> IVFIndex or None (dirty)
        self._mask_cache = {}      # table -> {(column, value): np.bool array}

    # ------------------------------------------------------------------
    # lifecycle
    # ------------------------------------------------------------------

    def finish_startup(self):
        """Post-recovery maintenance before the database reports ready."""
        for table in self.tables.values():
            if table.needs_compaction():
                table.compact()

    def shutdown(self):
        if self.txn is not None:
            self.txn = None  # uncommitted work is discarded
        for table in self.tables.values():
            if table.needs_compaction():
                table.compact()
            table.close()

    def _invalidate(self, name):
        self._row_cache.pop(name, None)
        self._vector_cache.pop(name, None)
        self._ann.pop(name, None)
        self._mask_cache.pop(name, None)

    # ------------------------------------------------------------------
    # statement dispatch
    # ------------------------------------------------------------------

    def execute(self, sql, search_mode):
        statement = sqlparser.parse_sql(sql)
        if isinstance(statement, sqlparser.CreateTable):
            return self._create_table(statement)
        if isinstance(statement, sqlparser.CreateVectorIndex):
            return self._create_vector_index(statement)
        if isinstance(statement, sqlparser.Insert):
            return {"affected_rows": self._insert(statement)}
        if isinstance(statement, sqlparser.Update):
            return {"affected_rows": self._update(statement)}
        if isinstance(statement, sqlparser.Delete):
            return {"affected_rows": self._delete(statement)}
        if isinstance(statement, sqlparser.Select):
            return self._select(statement, search_mode)
        if isinstance(statement, sqlparser.Begin):
            if self.txn is not None:
                raise DatabaseError("nested_transaction", "a transaction is already active")
            self.txn = Transaction()
            return {}
        if isinstance(statement, sqlparser.Commit):
            if self.txn is None:
                raise DatabaseError("no_active_transaction", "no transaction to commit")
            self._commit()
            return {}
        if isinstance(statement, sqlparser.Rollback):
            if self.txn is None:
                raise DatabaseError("no_active_transaction", "no transaction to roll back")
            self.txn = None
            return {}
        raise unsupported_sql("unsupported statement")

    # ------------------------------------------------------------------
    # DDL
    # ------------------------------------------------------------------

    def _create_table(self, statement):
        name = statement.name.lower()
        columns = [
            {
                "name": column["name"].lower(),
                "type": column["type"],
                "size": column["size"],
                "primary_key": bool(column.get("primary_key")),
            }
            for column in statement.columns
        ]
        self.schema.create_table(name, columns)
        table = TableFile(self.data_dir, name, columns)
        table.open()
        self.tables[name] = table
        return {}

    def _create_vector_index(self, statement):
        name = statement.table.lower()
        table = self._require_table(name)
        column = statement.column.lower()
        definition = next((c for c in table.columns if c["name"] == column), None)
        if definition is None:
            raise DatabaseError("column_not_found", f"unknown column {column!r}")
        if definition["type"] != "VECTOR":
            raise DatabaseError("type_mismatch", f"column {column!r} is not a VECTOR column")
        if self.schema.tables[name].get("vector_index"):
            raise DatabaseError("index_already_exists", "the table already has a vector index")
        self.schema.set_vector_index(name, statement.name.lower(), column)
        self._ann[name] = None  # rebuilt lazily on the first ann query
        return {}

    def _require_table(self, name):
        table = self.tables.get(name)
        if table is None:
            raise DatabaseError("table_not_found", f"unknown table {name!r}")
        return table

    # ------------------------------------------------------------------
    # value validation
    # ------------------------------------------------------------------

    def _validate_value(self, column, literal):
        kind = column["type"]
        if kind == "BIGINT":
            if isinstance(literal, bool) or not isinstance(literal, (int, float)):
                raise DatabaseError("type_mismatch", f"column {column['name']!r} expects BIGINT")
            if isinstance(literal, float):
                if not literal.is_integer():
                    raise DatabaseError("type_mismatch", f"column {column['name']!r} expects an integer")
                literal = int(literal)
            if not (I64_MIN <= literal <= I64_MAX):
                raise DatabaseError("value_out_of_range", "BIGINT value out of range")
            return literal
        if kind == "VARCHAR":
            if not isinstance(literal, str):
                raise DatabaseError("type_mismatch", f"column {column['name']!r} expects VARCHAR")
            if len(literal) > column["size"]:
                raise DatabaseError("value_out_of_range", f"VARCHAR({column['size']}) value too long")
            return literal
        # VECTOR
        if not isinstance(literal, str):
            raise DatabaseError("type_mismatch", f"column {column['name']!r} expects a vector literal")
        vector = parse_vector_text(literal, column["size"])
        return vector.tobytes()

    # ------------------------------------------------------------------
    # INSERT
    # ------------------------------------------------------------------

    def _insert(self, statement):
        name = statement.table.lower()
        table = self._require_table(name)
        columns = table.columns
        if statement.columns is not None:
            wanted = [c.lower() for c in statement.columns]
            if len(set(wanted)) != len(wanted):
                raise syntax_error("duplicate column in INSERT column list")
            by_name = {c["name"]: c for c in columns}
            unknown = [c for c in wanted if c not in by_name]
            if unknown:
                raise DatabaseError("column_not_found", f"unknown column {unknown[0]!r}")
            target = [by_name[c] for c in wanted]
        else:
            target = list(columns)
        pk_name = table.pk_column["name"]
        overlay = self.txn.overlay.setdefault(name, {}) if self.txn else None
        txn_entries = [] if self.txn else None
        seen_pks = set()
        encoded_rows = []
        for values in statement.rows:
            if len(values) != len(target):
                raise DatabaseError("column_count_mismatch", "INSERT value count does not match columns")
            row = {}
            for column, literal in zip(target, values):
                row[column["name"]] = self._validate_value(column, literal)
            if pk_name not in row:
                raise DatabaseError("column_count_mismatch", "missing primary key value")
            pk = row[pk_name]
            if pk in seen_pks:
                raise DatabaseError("duplicate_key", "duplicate primary key in INSERT")
            seen_pks.add(pk)
            if overlay is not None:
                # Visible duplicates: pending row, pending non-delete, or base row.
                if pk in overlay and overlay[pk] is not DELETED:
                    raise DatabaseError("duplicate_key", "duplicate primary key")
                if pk not in overlay and pk in table.index:
                    raise DatabaseError("duplicate_key", "duplicate primary key")
            elif pk in table.index:
                raise DatabaseError("duplicate_key", "duplicate primary key")
            encoded_rows.append((pk, row))
        records = []
        for pk, row in encoded_rows:
            record = table.encode_upsert(row)
            if self.txn:
                txn_entries.append((record, OP_UPSERT, pk))
                overlay[pk] = row
            else:
                records.append((record, OP_UPSERT, pk))
        if self.txn:
            self.txn.entries.setdefault(name, []).extend(txn_entries)
            self.txn.touched.add(name)
        else:
            table.append_records(records)
            self._invalidate(name)
            if table.needs_compaction():
                table.compact()
        return len(encoded_rows)

    # ------------------------------------------------------------------
    # UPDATE / DELETE
    # ------------------------------------------------------------------

    def _update(self, statement):
        name = statement.table.lower()
        table = self._require_table(name)
        by_name = {c["name"]: c for c in table.columns}
        assignments = []
        for column_name, literal in statement.assignments:
            column_name = column_name.lower()
            column = by_name.get(column_name)
            if column is None:
                raise DatabaseError("column_not_found", f"unknown column {column_name!r}")
            if column.get("primary_key"):
                raise unsupported_sql("updating the primary key is not supported")
            assignments.append((column, self._validate_value(column, literal)))
        condition = self._compile_condition(statement.where, table)
        matches = self._matching_pks(table, condition)
        overlay = self.txn.overlay.setdefault(name, {}) if self.txn else None
        txn_entries = []
        direct = []
        for pk in matches:
            current = self._current_row(table, pk)
            if current is None:
                continue
            row = dict(current)
            for column, value in assignments:
                row[column["name"]] = value
            record = table.encode_upsert(row)
            if self.txn:
                txn_entries.append((record, OP_UPSERT, pk))
                overlay[pk] = row
            else:
                direct.append((record, OP_UPSERT, pk))
        count = len(txn_entries) if self.txn else len(direct)
        if self.txn:
            self.txn.entries.setdefault(name, []).extend(txn_entries)
            self.txn.touched.add(name)
        elif direct:
            table.append_records(direct)
            self._invalidate(name)
            if table.needs_compaction():
                table.compact()
        return count

    def _delete(self, statement):
        name = statement.table.lower()
        table = self._require_table(name)
        condition = self._compile_condition(statement.where, table)
        matches = self._matching_pks(table, condition)
        overlay = self.txn.overlay.setdefault(name, {}) if self.txn else None
        txn_entries = []
        direct = []
        for pk in matches:
            record = table.encode_delete(pk)
            if self.txn:
                txn_entries.append((record, OP_DELETE, pk))
                overlay[pk] = DELETED
            else:
                direct.append((record, OP_DELETE, pk))
        count = len(txn_entries) if self.txn else len(direct)
        if self.txn:
            self.txn.entries.setdefault(name, []).extend(txn_entries)
            self.txn.touched.add(name)
        elif direct:
            table.append_records(direct)
            self._invalidate(name)
            if table.needs_compaction():
                table.compact()
        return count

    # ------------------------------------------------------------------
    # transactions
    # ------------------------------------------------------------------

    def _commit(self):
        txn = self.txn
        self.txn = None
        for name in txn.touched:
            table = self.tables.get(name)
            if table is None:
                continue
            entries = txn.entries.get(name, [])
            if entries:
                table.append_records(entries)
            self._invalidate(name)
            if table.needs_compaction():
                table.compact()

    # ------------------------------------------------------------------
    # row access helpers
    # ------------------------------------------------------------------

    def _current_row(self, table, pk):
        """Latest visible row for a pk, honoring the active transaction."""
        if self.txn is not None:
            overlay = self.txn.overlay.get(table.name)
            if overlay is not None and pk in overlay:
                value = overlay[pk]
                return None if value is DELETED else dict(value)
        row = table.read_row(pk)
        return dict(row) if row is not None else None

    def _matching_pks(self, table, condition):
        """Primary keys of rows matching ``condition`` (None matches all)."""
        if condition is not None and condition.point_lookup:
            pk = condition.point_pk
            if self.txn is not None:
                overlay = self.txn.overlay.get(table.name, {})
                if pk in overlay:
                    return [] if overlay[pk] is DELETED else [pk]
            return [pk] if pk in table.index else []
        matches = []
        for pk, row in self._iter_rows(table):
            if condition is None or condition.match(row):
                matches.append(pk)
        return matches

    def _iter_rows(self, table):
        """Yield every visible (pk, row), including transaction-local rows."""
        if self.txn is not None:
            overlay = self.txn.overlay.get(table.name, {})
            pending = set(overlay)
            for pk, row in table.scan_latest():
                if pk in pending:
                    continue
                yield pk, row
            for pk, value in overlay.items():
                if value is not DELETED:
                    yield pk, dict(value)
            return
        cache = self._row_cache.get(table.name)
        if cache is None and table.file_size <= ROW_CACHE_LIMIT:
            cache = dict(table.scan_latest())
            self._row_cache[table.name] = cache
        if cache is not None:
            yield from cache.items()
            return
        yield from table.scan_latest()

    # ------------------------------------------------------------------
    # condition compilation
    # ------------------------------------------------------------------

    def _compile_condition(self, where, table):
        if where is None:
            return None
        by_name = {c["name"]: c for c in table.columns}
        checks = []
        for term in where.terms:
            column_name = term.column.lower()
            column = by_name.get(column_name)
            if column is None:
                raise DatabaseError("column_not_found", f"unknown column {column_name!r}")
            literal = term.literal
            op = term.op
            if column["type"] == "VECTOR":
                if op not in ("=", "!="):
                    raise DatabaseError("type_mismatch", "vectors only support equality comparison")
                if not isinstance(literal, str):
                    raise DatabaseError("type_mismatch", "vector comparison expects a vector literal")
                vector_bytes = parse_vector_text(literal, column["size"]).tobytes()
                checks.append(("vector", column["name"], op, vector_bytes))
            elif column["type"] == "BIGINT":
                if isinstance(literal, str):
                    raise DatabaseError("type_mismatch", "BIGINT comparison expects a number")
                checks.append(("number", column["name"], op, literal))
            else:  # VARCHAR
                if not isinstance(literal, str):
                    raise DatabaseError("type_mismatch", "VARCHAR comparison expects a string")
                checks.append(("string", column["name"], op, literal))
        return Condition(checks, table.pk_column["name"])

    def _equality_mask(self, table, checks):
        """Boolean mask over cached pks for a conjunction of equality terms.

        Returns None when any term cannot be resolved through the mask cache;
        the caller then falls back to per-row evaluation.
        """
        if self.txn is not None:
            return None
        cache = self._mask_cache.setdefault(table.name, {})
        pks, _, _, _ = self._get_vector_cache(table)
        mask = None
        for kind, column, op, literal in checks:
            if op != "=":
                return None
            if kind == "vector":
                return None
            key = (column, literal)
            cached = cache.get(key)
            if cached is None:
                rows = self._row_cache.get(table.name)
                if rows is None:
                    return None
                if kind == "number":
                    cached = np.fromiter(
                        (rows[pk][column] == literal for pk in pks),
                        dtype=bool, count=len(pks),
                    )
                else:
                    cached = np.fromiter(
                        (rows[pk][column] == literal for pk in pks),
                        dtype=bool, count=len(pks),
                    )
                cache[key] = cached
            mask = cached.copy() if mask is None else (mask & cached)
        return mask


class Condition:
    """A compiled conjunction of column comparisons."""

    __slots__ = ("checks", "point_lookup", "point_pk")

    def __init__(self, checks, pk_name):
        self.checks = checks
        self.point_lookup = False
        self.point_pk = None
        if len(checks) == 1 and checks[0][0] == "number" and checks[0][1] == pk_name \
                and checks[0][2] == "=" and isinstance(checks[0][3], int):
            self.point_lookup = True
            self.point_pk = checks[0][3]

    def match(self, row):
        for kind, column, op, literal in self.checks:
            value = row.get(column)
            if kind == "vector":
                if (op == "=") != (value == literal):
                    return False
            elif not _compare(op, value, literal):
                return False
        return True


def _compare(op, left, right):
    if op == "=":
        return left == right
    if op == "!=":
        return left != right
    if op == "<":
        return left < right
    if op == "<=":
        return left <= right
    if op == ">":
        return left > right
    return left >= right


class _Desc:
    """Inverts comparison order for descending heap keys."""

    __slots__ = ("value",)

    def __init__(self, value):
        self.value = value

    def __lt__(self, other):
        return other.value < self.value

    def __eq__(self, other):
        return self.value == other.value


# ----------------------------------------------------------------------
# vector caches
# ----------------------------------------------------------------------

def _engine_vector_cache(self, table):
    """(pks ascending, pk->position, float32 matrix, squared norms) for vectors."""
    cached = self._vector_cache.get(table.name)
    if cached is not None:
        return cached
    vector_column = next(c for c in table.columns if c["type"] == "VECTOR")
    if table.file_size > ROW_CACHE_LIMIT:
        self._vector_cache[table.name] = None  # too large: always stream
        return None
    pairs = []
    for pk, row in table.scan_latest():
        pairs.append((pk, row[vector_column["name"]]))
    pairs.sort(key=lambda item: item[0])
    if not pairs:
        result = ([], {}, None, None)
    else:
        pks = [pk for pk, _ in pairs]
        positions = {pk: i for i, pk in enumerate(pks)}
        matrix = np.vstack([np.frombuffer(data, dtype="<f4") for _, data in pairs])
        result = (pks, positions, matrix, (matrix * matrix).sum(axis=1))
    self._vector_cache[table.name] = result
    return result


Engine._get_vector_cache = _engine_vector_cache


def _engine_fetch_vector(self, table, pk):
    row = self._current_row(table, pk)
    if row is None:
        return None
    vector_column = next(c for c in table.columns if c["type"] == "VECTOR")
    data = row[vector_column["name"]]
    return np.frombuffer(data, dtype="<f4").astype(np.float64)


Engine._fetch_vector = _engine_fetch_vector


# ----------------------------------------------------------------------
# SELECT
# ----------------------------------------------------------------------

def _engine_select(self, statement, search_mode):
    name = statement.table.lower()
    table = self._require_table(name)
    vector_column = next((c for c in table.columns if c["type"] == "VECTOR"), None)
    condition = self._compile_condition(statement.where, table)

    # Resolve the projection.
    star = statement.items == "*"
    items = statement.items if not star else []
    if not star:
        for item in items:
            if item.kind == "column":
                column = next((c for c in table.columns if c["name"] == item.column), None)
                if column is None:
                    raise DatabaseError("column_not_found", f"unknown column {item.column!r}")
            else:
                vcol, second, literal = item.distance
                if vcol.lower() not in {c["name"] for c in table.columns}:
                    raise DatabaseError("column_not_found", f"unknown column {vcol!r}")
                if second is not None and second.lower() not in {c["name"] for c in table.columns}:
                    raise DatabaseError("column_not_found", f"unknown column {second!r}")
                if vector_column is None:
                    raise DatabaseError("column_not_found", "table has no VECTOR column")

    # Ordering by distance: either a projected L2_DISTANCE item referenced by
    # alias/ordinal/name, or an L2_DISTANCE expression in ORDER BY itself.
    sort_distance = self._resolve_order_distance(statement, items, vector_column)
    if sort_distance is not None and statement.order_by:
        return self._topk_select(table, statement, star, items, sort_distance,
                                 condition, search_mode)
    return self._plain_select(table, statement, star, items, condition)


Engine._select = _engine_select


def _resolve_order_distance(self, statement, items, vector_column):
    if vector_column is None or not statement.order_by:
        return None
    first_key, _ = statement.order_by[0]
    if isinstance(first_key, tuple):  # ("distance", (vcol, second, literal))
        return first_key[1]
    if isinstance(first_key, int):
        if items and 1 <= first_key <= len(items):
            item = items[first_key - 1]
            if item.kind == "l2_distance":
                return item.distance
        return None
    key = first_key.lower()
    for item in items:
        if item.kind != "l2_distance":
            continue
        if item.alias:
            if item.alias.lower() == key:
                return item.distance
        elif key.startswith("l2_distance"):
            return item.distance
    return None


Engine._resolve_order_distance = _resolve_order_distance


def _output_names(self, table, star, items):
    if star:
        return [c["name"] for c in table.columns]
    names = []
    for item in items:
        if item.kind == "column":
            names.append(item.alias or item.column)
        else:
            names.append(item.alias or "L2_DISTANCE")
    return names


Engine._output_names = _output_names


def _project_row(self, table, star, items, row, distance_value):
    """Build one output row; vector columns become lists of floats."""
    if star:
        out = []
        for column in table.columns:
            value = row[column["name"]]
            if column["type"] == "VECTOR":
                out.append(np.frombuffer(value, dtype="<f4").tolist())
            else:
                out.append(value)
        return out
    out = []
    for item in items:
        if item.kind == "column":
            value = row[item.column]
            if isinstance(value, bytes):
                out.append(np.frombuffer(value, dtype="<f4").tolist())
            else:
                out.append(value)
        else:
            out.append(distance_value)
    return out


Engine._project_row = _project_row


def _engine_plain_select(self, table, statement, star, items, condition):
    names = self._output_names(table, star, items)
    column_names = [c["name"] for c in table.columns]
    entries = []  # (pk, source_row, output_row)
    for pk, row in self._iter_rows(table):
        if condition is not None and not condition.match(row):
            continue
        entries.append((pk, row, self._project_row(table, star, items, row, None)))

    order_by = statement.order_by
    if order_by:
        output_names = names
        star_names = {name: i for i, name in enumerate(column_names)}
        specs = []  # (source, ref, desc)
        for key, desc in order_by:
            if isinstance(key, int):
                if not 1 <= key <= len(output_names):
                    raise DatabaseError("column_not_found", f"ORDER BY position {key} out of range")
                specs.append(("output", key - 1, desc))
                continue
            key_lower = key.lower()
            for i, out_name in enumerate(output_names):
                if out_name.lower() == key_lower:
                    specs.append(("output", i, desc))
                    break
            else:
                if key_lower in star_names:
                    specs.append(("column", key_lower, desc))
                else:
                    raise DatabaseError("column_not_found", f"unknown ORDER BY column {key!r}")

        # Stable multi-key sort: pk ascending first, then keys last-to-first.
        entries.sort(key=lambda entry: entry[0])
        for source, ref, desc in reversed(specs):
            if source == "output":
                entries.sort(key=lambda entry, i=ref: entry[2][i], reverse=desc)
            else:
                entries.sort(key=lambda entry, i=ref: entry[1][i], reverse=desc)
        if statement.limit is not None:
            entries = entries[:statement.limit]
    elif statement.limit is not None:
        entries = entries[:statement.limit]
    return {"columns": names, "rows": [out for _, _, out in entries]}


Engine._plain_select = _engine_plain_select


# ----------------------------------------------------------------------
# TopK (ORDER BY distance ... LIMIT K)
# ----------------------------------------------------------------------

def _engine_topk_select(self, table, statement, star, items, sort_distance,
                        condition, search_mode):
    names = self._output_names(table, star, items)
    vcol_name, second_column, literal = sort_distance
    vector_column = next(c for c in table.columns if c["type"] == "VECTOR")
    if vcol_name.lower() != vector_column["name"]:
        raise DatabaseError("column_not_found", f"unknown vector column {vcol_name!r}")
    dim = vector_column["size"]
    if literal is None:
        raise DatabaseError("type_mismatch", "L2_DISTANCE against a column is not supported here")
    query = parse_vector_text(literal, dim).astype(np.float64)
    limit = statement.limit
    order_desc = bool(statement.order_by and statement.order_by[0][1])

    # Which output column (if any) shows the distance value.
    distance_output_index = None
    if not star:
        for offset, item in enumerate(items):
            if item.kind == "l2_distance" and item.distance == sort_distance:
                distance_output_index = offset
                break

    if self.txn is None and table.file_size <= ROW_CACHE_LIMIT:
        pairs = self._topk_cached(table, query, limit, condition, search_mode,
                                  desc=order_desc)
    else:
        pairs = self._topk_streamed(table, query, limit, condition, search_mode,
                                    desc=order_desc)

    rows_out = []
    for dist, pk in pairs:
        row = self._current_row(table, pk)
        if row is None:
            continue
        value = float(dist) if distance_output_index is not None else None
        rows_out.append(self._project_row(table, star, items, row, value))
    return {"columns": names, "rows": rows_out}


Engine._topk_select = _engine_topk_select


def _engine_topk_cached(self, table, query, limit, condition, search_mode,
                        desc=False, allowed=None):
    """Exact TopK: float32 pre-ranking narrows candidates, float64 exact
    distances decide the final order, so ties and values stay exact."""
    if limit is not None and limit <= 0:
        return []
    cached = self._get_vector_cache(table)
    if cached is None:
        return self._topk_streamed(table, query, limit, condition, search_mode,
                                   desc=desc, allowed=allowed)
    pks, _, matrix, sqnorms = cached
    count = len(pks)
    if count == 0:
        return []
    # Candidate mask: fast path for equality conjunctions, else row-wise.
    if condition is not None and allowed is None:
        mask = self._equality_mask(table, condition.checks)
        if mask is None:
            rows = self._row_cache.get(table.name)
            if rows is None:
                rows = dict(table.scan_latest())
                self._row_cache[table.name] = rows
            mask = np.fromiter(
                (condition.match(rows[pk]) for pk in pks), dtype=bool, count=count
            )
    elif allowed is not None:
        mask = np.fromiter((pk in allowed for pk in pks), dtype=bool, count=count)
    else:
        mask = None

    if mask is not None:
        candidate_positions = np.nonzero(mask)[0]
    else:
        candidate_positions = np.arange(count)
    n_candidates = len(candidate_positions)
    if n_candidates == 0:
        return []
    k = min(limit, n_candidates) if limit is not None else n_candidates

    if search_mode == "ann" and self._ann_enabled(table) and n_candidates > EXACT_SCAN_THRESHOLD:
        allowed_set = None if mask is None else {pks[i] for i in candidate_positions}
        return self._ann_search(table, query, k, allowed_set, desc=desc)

    if desc or k >= n_candidates:
        # Farthest-first or full result sets need exact distances for everyone.
        distances = chunk_l2_distances(matrix[candidate_positions].astype(np.float64), query)
        if desc:
            order = np.lexsort((candidate_positions, -distances))[:k]
        else:
            order = np.argsort(distances, kind="stable")
        return [(float(distances[i]), pks[candidate_positions[i]]) for i in order]

    # Phase 1: float32 approximation (||x||^2 - 2 x.q + ||q||^2) to pick a
    # superset of the true top-k; phase 2 re-ranks it exactly in float64.
    q32 = query.astype(np.float32)
    approx = sqnorms[candidate_positions] - 2.0 * (matrix[candidate_positions] @ q32) \
        + float(np.dot(q32, q32))
    top_m = min(n_candidates, max(4 * k, 2048))
    superset = np.argpartition(approx, top_m - 1)[:top_m]
    exact_positions = candidate_positions[superset]
    distances = chunk_l2_distances(matrix[exact_positions].astype(np.float64), query)
    # Candidates are pk-ascending, so a stable argsort resolves ties by pk.
    local_order = np.argsort(distances, kind="stable")[:k]
    return [
        (float(distances[j]), pks[exact_positions[j]])
        for j in local_order
    ]


Engine._topk_cached = _engine_topk_cached


def _engine_topk_streamed(self, table, query, limit, condition, search_mode,
                          desc=False, allowed=None):
    """Exact TopK with a bounded heap while streaming records from disk."""
    if limit is not None and limit <= 0:
        return []
    vector_column = next(c for c in table.columns if c["type"] == "VECTOR")
    dim = vector_column["size"]
    heap = []
    if desc:
        # Keep the k largest distances; root = worst (smallest dist, largest pk).
        for pk, row in self._iter_rows(table):
            if condition is not None and not condition.match(row):
                continue
            if allowed is not None and pk not in allowed:
                continue
            vector = np.frombuffer(row[vector_column["name"]], dtype="<f4").astype(np.float64)
            dist = l2_distance(vector, query)
            entry = (dist, -pk)
            if limit is None:
                heap.append(entry)
                continue
            if len(heap) < limit:
                heapq.heappush(heap, entry)
            elif entry > heap[0]:
                heapq.heapreplace(heap, entry)
        # entries (dist, -pk) -> (dist, pk), ordered by (-dist, pk).
        result = [(-d, -npk) for d, npk in heap]
        result.sort()
        return result
    for pk, row in self._iter_rows(table):
        if condition is not None and not condition.match(row):
            continue
        if allowed is not None and pk not in allowed:
            continue
        vector = np.frombuffer(row[vector_column["name"]], dtype="<f4").astype(np.float64)
        dist = l2_distance(vector, query)
        entry = (-dist, -pk)
        if limit is None:
            heap.append(entry)
            continue
        if len(heap) < limit:
            heapq.heappush(heap, entry)
        elif entry > heap[0]:
            heapq.heapreplace(heap, entry)
    result = [(-nd, -npk) for nd, npk in heap]
    result.sort()
    return result


Engine._topk_streamed = _engine_topk_streamed


def _engine_ann_enabled(self, table):
    """True when an index exists for this table."""
    return self.schema.tables[table.name].get("vector_index") is not None


Engine._ann_enabled = _engine_ann_enabled


def _engine_ann_search(self, table, query, k, allowed_set, desc=False):
    """ANN TopK via the IVF index, rebuilt lazily from current rows."""
    index = self._ann.get(table.name)
    if index is None:
        vector_column = next(c for c in table.columns if c["type"] == "VECTOR")
        index = IVFIndex.build(
            vector_column["size"],
            ((pk, np.frombuffer(row[vector_column["name"]], dtype="<f4"))
             for pk, row in table.scan_latest()),
        )
        self._ann[table.name] = index
    nprobe_ratio = 0.5  # generous probe ratio for recall; benchmark stays exact
    if allowed_set is None:
        pairs = index.search(query, k, nprobe_ratio, lambda pk: self._fetch_vector(table, pk))
    else:
        pairs = index.search_filtered(
            query, k, nprobe_ratio, lambda pk: self._fetch_vector(table, pk), allowed_set
        )
    if desc:
        pairs.sort(key=lambda pair: (-pair[0], pair[1]))
    return pairs


Engine._ann_search = _engine_ann_search
