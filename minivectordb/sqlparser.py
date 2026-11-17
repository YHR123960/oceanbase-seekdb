"""Tokenizer and recursive-descent parser for the MiniVectorDB SQL subset.

Supported statements:

- CREATE TABLE name (col TYPE ..., ...) with exactly one BIGINT PRIMARY KEY
- CREATE VECTOR INDEX name ON table (column)
- INSERT INTO name [(cols)] VALUES (...), (...)
- SELECT items FROM name [WHERE cond] [ORDER BY key [DESC], ...] [LIMIT n]
- UPDATE name SET col = value, ... [WHERE cond]
- DELETE FROM name [WHERE cond]
- BEGIN / COMMIT / ROLLBACK

Conditions are comparisons (=, <, <=, >, >=) between a column and a literal,
combined with AND and parentheses.  Anything else in the SQL language is
reported as an error rather than silently executed with another meaning.
"""

from errors import DatabaseError, syntax_error, unsupported_sql

KEYWORDS = {
    "CREATE", "TABLE", "VECTOR", "INDEX", "ON", "INSERT", "INTO", "VALUES",
    "SELECT", "FROM", "WHERE", "ORDER", "BY", "LIMIT", "UPDATE", "SET",
    "DELETE", "BEGIN", "COMMIT", "ROLLBACK", "AND", "AS", "ASC", "DESC",
    "PRIMARY", "KEY", "BIGINT", "VARCHAR", "L2_DISTANCE", "NOT", "NULL",
    "START", "TRANSACTION", "EXISTS", "IF", "DROP", "ALTER", "JOIN", "GROUP",
    "UNION", "DISTINCT", "OR", "IN", "LIKE", "COUNT", "INNER", "LEFT",
    "RIGHT", "OUTER", "HAVING", "TRUE", "FALSE",
}

COMPARISON_OPS = {"=", "<", "<=", ">", ">=", "!=", "<>"}


class Token:
    __slots__ = ("kind", "value", "upper")

    def __init__(self, kind, value):
        self.kind = kind  # 'ident', 'keyword', 'int', 'float', 'string', 'op', 'end'
        self.value = value
        self.upper = value.upper() if kind == "ident" else value

    def __repr__(self):
        return f"Token({self.kind},{self.value!r})"


def tokenize(text):
    tokens = []
    i, n = 0, len(text)
    while i < n:
        ch = text[i]
        if ch.isspace():
            i += 1
            continue
        if ch == "'":
            value, i = _read_string(text, i)
            tokens.append(Token("string", value))
            continue
        if ch.isdigit() or (ch == "." and i + 1 < n and text[i + 1].isdigit()):
            token, i = _read_number(text, i)
            tokens.append(token)
            continue
        if ch.isalpha() or ch == "_":
            j = i
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            word = text[i:j]
            kind = "keyword" if word.upper() in KEYWORDS else "ident"
            tokens.append(Token(kind, word))
            i = j
            continue
        two = text[i:i + 2]
        if two in ("<=", ">=", "<>", "!="):
            tokens.append(Token("op", two))
            i += 2
            continue
        if ch in "=<>*(),;+-":
            tokens.append(Token("op", ch))
            i += 1
            continue
        raise syntax_error(f"unexpected character {ch!r}")
    tokens.append(Token("end", ""))
    return tokens


def _read_string(text, i):
    # text[i] is the opening quote; '' escapes a quote.
    parts = []
    i += 1
    n = len(text)
    while True:
        if i >= n:
            raise syntax_error("unterminated string literal")
        ch = text[i]
        if ch == "'":
            if i + 1 < n and text[i + 1] == "'":
                parts.append("'")
                i += 2
                continue
            return "".join(parts), i + 1
        parts.append(ch)
        i += 1


def _read_number(text, i):
    n = len(text)
    j = i
    seen_dot = False
    seen_exp = False
    while j < n:
        ch = text[j]
        if ch.isdigit():
            j += 1
        elif ch == "." and not seen_dot and not seen_exp:
            seen_dot = True
            j += 1
        elif ch in "eE" and not seen_exp and j > i:
            seen_exp = True
            j += 1
            if j < n and text[j] in "+-":
                j += 1
        else:
            break
    body = text[i:j]
    if seen_dot or seen_exp:
        try:
            value = float(body)
        except ValueError:
            raise syntax_error(f"bad number literal {body!r}") from None
        return Token("float", value), j
    return Token("int", int(body)), j


# ---------------------------------------------------------------------------
# AST containers
# ---------------------------------------------------------------------------


class CreateTable:
    def __init__(self, name, columns):
        self.name = name
        self.columns = columns  # dicts: name/type/size/primary_key


class CreateVectorIndex:
    def __init__(self, name, table, column):
        self.name = name
        self.table = table
        self.column = column


class Insert:
    def __init__(self, table, columns, rows):
        self.table = table
        self.columns = columns  # list or None
        self.rows = rows  # list of list of literal values


class SelectItem:
    def __init__(self, kind, column, distance, alias):
        self.kind = kind  # 'column' or 'l2_distance'
        self.column = column
        # distance: (vector_column, second_vector_column_or_None, literal_or_None)
        self.distance = distance
        self.alias = alias


class Select:
    def __init__(self, table, items, where, order_by, limit):
        self.table = table
        self.items = items  # list of SelectItem, or the string '*'
        self.where = where
        self.order_by = order_by  # list of (key, desc)
        self.limit = limit


class Update:
    def __init__(self, table, assignments, where):
        self.table = table
        self.assignments = assignments  # list of (column, literal)
        self.where = where


class Delete:
    def __init__(self, table, where):
        self.table = table
        self.where = where


class Begin:
    pass


class Commit:
    pass


class Rollback:
    pass


class Compare:
    __slots__ = ("column", "op", "literal")

    def __init__(self, column, op, literal):
        self.column = column
        self.op = op
        self.literal = literal


class AndCondition:
    __slots__ = ("terms",)

    def __init__(self, terms):
        self.terms = terms


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------


class Parser:
    def __init__(self, text):
        self.tokens = tokenize(text)
        self.pos = 0

    # -- token helpers ------------------------------------------------------

    def peek(self):
        return self.tokens[self.pos]

    def advance(self):
        token = self.tokens[self.pos]
        self.pos += 1
        return token

    def expect_keyword(self, word):
        token = self.peek()
        if token.kind != "keyword" or token.upper != word:
            raise syntax_error(f"expected {word}, got {token.value!r}")
        return self.advance()

    def expect_op(self, op):
        token = self.peek()
        if token.kind != "op" or token.value != op:
            raise syntax_error(f"expected {op!r}, got {token.value!r}")
        return self.advance()

    def expect_ident(self, what="identifier"):
        token = self.peek()
        if token.kind != "ident":
            raise syntax_error(f"expected {what}, got {token.value!r}")
        return self.advance().value

    def at_keyword(self, word):
        token = self.peek()
        return token.kind == "keyword" and token.upper == word

    def at_op(self, op):
        token = self.peek()
        return token.kind == "op" and token.value == op

    def expect_end(self):
        if self.at_op(";"):
            self.advance()
        token = self.peek()
        if token.kind != "end":
            raise syntax_error(f"unexpected token {token.value!r}")

    # -- statements ---------------------------------------------------------

    def parse_statement(self):
        token = self.peek()
        if token.kind != "keyword":
            raise syntax_error(f"unexpected token {token.value!r}")
        word = token.upper
        if word == "CREATE":
            statement = self.parse_create()
        elif word == "INSERT":
            statement = self.parse_insert()
        elif word == "SELECT":
            statement = self.parse_select()
        elif word == "UPDATE":
            statement = self.parse_update()
        elif word == "DELETE":
            statement = self.parse_delete()
        elif word == "BEGIN":
            self.advance()
            statement = Begin()
        elif word == "START":
            self.advance()
            self.expect_keyword("TRANSACTION")
            statement = Begin()
        elif word == "COMMIT":
            self.advance()
            statement = Commit()
        elif word == "ROLLBACK":
            self.advance()
            statement = Rollback()
        elif word in ("ALTER", "DROP"):
            raise unsupported_sql(f"{word} is not supported")
        else:
            raise unsupported_sql(f"statement {word} is not supported")
        self.expect_end()
        return statement

    # -- DDL ----------------------------------------------------------------

    def parse_create(self):
        self.expect_keyword("CREATE")
        if self.at_keyword("VECTOR"):
            self.advance()
            self.expect_keyword("INDEX")
            name = self.expect_ident("index name")
            self.expect_keyword("ON")
            table = self.expect_ident("table name")
            self.expect_op("(")
            column = self.expect_ident("column name")
            self.expect_op(")")
            return CreateVectorIndex(name, table, column)
        self.expect_keyword("TABLE")
        name = self.expect_ident("table name")
        self.expect_op("(")
        columns = []
        pk_count = 0
        while True:
            column_name = self.expect_ident("column name")
            column = self.parse_column_type(column_name)
            if self.at_keyword("PRIMARY"):
                self.advance()
                self.expect_keyword("KEY")
                column["primary_key"] = True
            if column.get("primary_key"):
                pk_count += 1
            columns.append(column)
            if self.at_op(","):
                self.advance()
                continue
            break
        self.expect_op(")")
        if self.at_keyword("PRIMARY"):
            self.advance()
            self.expect_keyword("KEY")
            self.expect_op("(")
            pk_name = self.expect_ident("primary key column")
            self.expect_op(")")
            for column in columns:
                if column["name"] == pk_name:
                    if column.get("primary_key"):
                        raise syntax_error("primary key declared twice")
                    column["primary_key"] = True
                    pk_count += 1
                    break
            else:
                raise syntax_error(f"primary key column {pk_name!r} not defined")
        if pk_count != 1:
            raise syntax_error("table must define exactly one BIGINT primary key")
        for column in columns:
            if column.get("primary_key") and column["type"] != "BIGINT":
                raise syntax_error("primary key column must be BIGINT")
        vector_columns = [c for c in columns if c["type"] == "VECTOR"]
        if len(vector_columns) > 1:
            raise syntax_error("at most one VECTOR column per table")
        return CreateTable(name, columns)

    def parse_column_type(self, column_name):
        token = self.advance()
        if token.kind != "keyword":
            raise syntax_error(f"expected column type, got {token.value!r}")
        word = token.upper
        if word == "BIGINT":
            return {"name": column_name, "type": "BIGINT", "size": None}
        if word == "VARCHAR":
            self.expect_op("(")
            size_token = self.advance()
            if size_token.kind != "int":
                raise syntax_error("VARCHAR length must be an integer")
            size = size_token.value
            if size <= 0:
                raise syntax_error("VARCHAR length must be positive")
            self.expect_op(")")
            return {"name": column_name, "type": "VARCHAR", "size": size}
        if word == "VECTOR":
            self.expect_op("(")
            dim_token = self.advance()
            if dim_token.kind != "int":
                raise syntax_error("VECTOR dimension must be an integer")
            dim = dim_token.value
            if dim <= 0:
                raise syntax_error("VECTOR dimension must be positive")
            self.expect_op(")")
            return {"name": column_name, "type": "VECTOR", "size": dim}
        raise syntax_error(f"unsupported column type {word}")

    # -- DML ----------------------------------------------------------------

    def parse_insert(self):
        self.expect_keyword("INSERT")
        self.expect_keyword("INTO")
        table = self.expect_ident("table name")
        columns = None
        if self.at_op("("):
            self.advance()
            columns = [self.expect_ident("column name")]
            while self.at_op(","):
                self.advance()
                columns.append(self.expect_ident("column name"))
            self.expect_op(")")
        self.expect_keyword("VALUES")
        rows = [self.parse_value_row()]
        while self.at_op(","):
            self.advance()
            rows.append(self.parse_value_row())
        return Insert(table, columns, rows)

    def parse_value_row(self):
        self.expect_op("(")
        values = [self.parse_literal()]
        while self.at_op(","):
            self.advance()
            values.append(self.parse_literal())
        self.expect_op(")")
        return values

    def parse_literal(self):
        token = self.peek()
        if token.kind == "op" and token.value in ("-", "+"):
            self.advance()
            number = self.advance()
            if number.kind not in ("int", "float"):
                raise syntax_error("expected number after sign")
            return -number.value if token.value == "-" else number.value
        if token.kind in ("int", "float", "string"):
            self.advance()
            return token.value
        if token.kind == "keyword" and token.upper in ("TRUE", "FALSE"):
            raise syntax_error("boolean literals are not supported")
        if token.kind == "keyword" and token.upper == "NULL":
            raise syntax_error("NULL is not supported")
        raise syntax_error(f"expected literal, got {token.value!r}")

    def parse_select(self):
        self.expect_keyword("SELECT")
        items = self.parse_select_items()
        self.expect_keyword("FROM")
        table = self.expect_ident("table name")
        where = self.parse_where_clause()
        order_by = []
        if self.at_keyword("ORDER"):
            self.advance()
            self.expect_keyword("BY")
            order_by = self.parse_order_list()
        limit = None
        if self.at_keyword("LIMIT"):
            self.advance()
            limit_token = self.advance()
            if limit_token.kind != "int":
                raise syntax_error("LIMIT must be a non-negative integer")
            limit = limit_token.value
        return Select(table, items, where, order_by, limit)

    def parse_select_items(self):
        if self.at_op("*"):
            self.advance()
            return "*"
        items = [self.parse_select_item()]
        while self.at_op(","):
            self.advance()
            items.append(self.parse_select_item())
        return items

    def parse_select_item(self):
        token = self.peek()
        if token.kind == "keyword" and token.upper == "L2_DISTANCE":
            self.advance()
            self.expect_op("(")
            column = self.expect_ident("vector column")
            self.expect_op(",")
            second = self.peek()
            if second.kind == "ident":
                self.advance()
                distance = (column, second.value, None)
            elif second.kind == "string":
                self.advance()
                distance = (column, None, second.value)
            else:
                raise syntax_error("L2_DISTANCE expects a vector literal or column")
            self.expect_op(")")
            alias = None
            if self.at_keyword("AS"):
                self.advance()
                alias = self.expect_ident("alias")
            return SelectItem("l2_distance", None, distance, alias)
        if token.kind == "ident":
            self.advance()
            alias = None
            if self.at_keyword("AS"):
                self.advance()
                alias = self.expect_ident("alias")
            return SelectItem("column", token.value, None, alias)
        raise syntax_error(f"unsupported select item {token.value!r}")

    def parse_order_list(self):
        entries = []
        while True:
            token = self.peek()
            if token.kind == "keyword" and token.upper == "L2_DISTANCE":
                distance = self.parse_distance_args()
                desc = False
                if self.at_keyword("DESC"):
                    self.advance()
                    desc = True
                elif self.at_keyword("ASC"):
                    self.advance()
                entries.append((("distance", distance), desc))
            elif token.kind == "int":
                self.advance()
                key = token.value
                desc = self.parse_order_direction()
                entries.append((key, desc))
            elif token.kind == "ident":
                self.advance()
                key = token.value
                desc = self.parse_order_direction()
                entries.append((key, desc))
            else:
                raise syntax_error(f"expected ORDER BY key, got {token.value!r}")
            if self.at_op(","):
                self.advance()
                continue
            break
        return entries

    def parse_order_direction(self):
        if self.at_keyword("DESC"):
            self.advance()
            return True
        if self.at_keyword("ASC"):
            self.advance()
        return False

    def parse_distance_args(self):
        """Parse L2_DISTANCE ( column , literal-or-column ) returning its parts."""
        self.advance()  # L2_DISTANCE keyword
        self.expect_op("(")
        column = self.expect_ident("vector column")
        self.expect_op(",")
        second = self.peek()
        if second.kind == "ident":
            self.advance()
            parts = (column, second.value, None)
        elif second.kind == "string":
            self.advance()
            parts = (column, None, second.value)
        else:
            raise syntax_error("L2_DISTANCE expects a vector literal or column")
        self.expect_op(")")
        return parts

    def parse_where_clause(self):
        if self.at_keyword("WHERE"):
            self.advance()
            return self.parse_condition()
        return None

    def parse_condition(self):
        terms = [self.parse_term()]
        while self.at_keyword("AND"):
            self.advance()
            terms.append(self.parse_term())
        return AndCondition(terms)

    def parse_term(self):
        if self.at_op("("):
            self.advance()
            inner = self.parse_condition()
            self.expect_op(")")
            if len(inner.terms) == 1:
                return inner.terms[0]
            return inner
        column = self.expect_ident("column name")
        token = self.advance()
        if token.kind != "op" or token.value not in COMPARISON_OPS:
            raise syntax_error(f"expected comparison operator, got {token.value!r}")
        op = token.value
        if op == "<>":
            op = "!="
        literal = self.parse_literal()
        return Compare(column, op, literal)

    def parse_update(self):
        self.expect_keyword("UPDATE")
        table = self.expect_ident("table name")
        self.expect_keyword("SET")
        assignments = []
        while True:
            column = self.expect_ident("column name")
            self.expect_op("=")
            value = self.parse_literal()
            assignments.append((column, value))
            if self.at_op(","):
                self.advance()
                continue
            break
        where = self.parse_where_clause()
        return Update(table, assignments, where)

    def parse_delete(self):
        self.expect_keyword("DELETE")
        self.expect_keyword("FROM")
        table = self.expect_ident("table name")
        where = self.parse_where_clause()
        return Delete(table, where)


def parse_sql(text):
    """Parse one SQL statement, rejecting empty input."""
    stripped = text.strip()
    if not stripped:
        raise syntax_error("empty statement")
    return Parser(stripped).parse_statement()
