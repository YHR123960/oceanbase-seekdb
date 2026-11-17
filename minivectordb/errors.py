"""Database error types with protocol error codes."""


class DatabaseError(Exception):
    """An error that is reported to the client with a semantic code."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def syntax_error(message="syntax error"):
    return DatabaseError("syntax_error", message)


def unsupported_sql(message="unsupported statement"):
    return DatabaseError("unsupported_sql", message)
