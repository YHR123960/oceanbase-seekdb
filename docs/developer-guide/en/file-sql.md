# Local file SQL

Use a session with the MySQL FILE privilege. Set `secure_file_priv` to the
directory that contains the data and enable file SQL in the session.

```sql
SET SESSION enable_file_sql = ON;
SELECT id, name FROM read_csv('/file_sql/customers.csv', header = true) ORDER BY id;
DESCRIBE SELECT * FROM read_ndjson('/file_sql/events.jsonl');
SELECT file FROM glob('/file_sql/*.csv') ORDER BY file;
COPY (SELECT id, name FROM customers ORDER BY id)
  TO '/file_sql/export.csv' (FORMAT CSV, HEADER, NULLSTR 'NULL');
```

Use absolute POSIX paths. Symbolic links and dot path components are rejected.
`glob` matches patterns in the last path component and returns regular file
paths. CSV accepts `header`, `delimiter`, `quote`, `escape`, and `nullstr`.
CSV and JSONL infer the schema from the complete file (`sample_size = -1`).
A missing JSON field and an unquoted empty CSV field are NULL. Integer and
floating-point columns widen to DOUBLE, dates and datetimes to DATETIME,
and other type conflicts to VARCHAR. A column with only NULL values retains
the NULL type. COPY accepts the same CSV dialect options and fails if its
destination already exists.

The EL9 x86-64 dependency profile used by Ubuntu 22.04 installs the OceanBase
Arrow and Parquet packages. Other POSIX CMake builds can supply the Arrow and
Parquet C++ packages (16 or later) through `CMAKE_PREFIX_PATH`.

```sh
./build.sh release --init -DSEEKDB_FILE_SQL_TESTS=ON
cmake --build build_release --target test_file_sql_parser test_file_scan_utils -j2
./build_release/src/sql/test_file_sql_parser
./build_release/src/sql/test_file_scan_utils
cmake --build build_release --target seekdb -j2
```

From the repository's deployment directory, run `./obd.sh mysqltest -n test
--suite file_sql` against a test deployment built from this code.
