"""Vector literal parsing, float32 semantics and distance helpers.

Vectors are fixed-dimension float32 arrays.  Input values must be finite and
representable in float32; NaN, Infinity, malformed text and conversion
overflow are rejected.  Distances are computed over the stored float32
values in float64 arithmetic and returned as finite doubles.
"""

import math

import numpy as np

from errors import DatabaseError


def parse_vector_text(text, dim):
    """Parse a vector literal such as '[1, 0, 0, 0]' into a float32 array."""
    body = text.strip()
    if not (body.startswith("[") and body.endswith("]")):
        raise DatabaseError("invalid_vector", "vector literal must be an array")
    body = body[1:-1].strip()
    values = []
    if body:
        for piece in body.split(","):
            piece = piece.strip()
            if not piece:
                raise DatabaseError("invalid_vector", "empty vector component")
            try:
                number = float(piece)
            except ValueError:
                raise DatabaseError("invalid_vector", "invalid vector component: " + piece) from None
            if math.isnan(number) or math.isinf(number):
                raise DatabaseError("invalid_vector", "vector components must be finite")
            values.append(number)
    if dim is not None and len(values) != dim:
        raise DatabaseError(
            "dimension_mismatch",
            f"vector has {len(values)} dimensions, expected {dim}",
        )
    array = np.asarray(values, dtype=np.float64)
    return to_float32(array, dim)


def to_float32(array, dim=None):
    """Convert a float64 array to float32, rejecting conversion overflow."""
    result = array.astype(np.float32)
    if result.shape:
        if not np.all(np.isfinite(result)):
            raise DatabaseError("value_out_of_range", "vector component overflow in float32")
    elif not np.isfinite(result):
        raise DatabaseError("value_out_of_range", "vector component overflow in float32")
    return result


def encode_vector(array):
    return np.asarray(array, dtype=np.float32).tobytes()


def decode_vector(payload, dim):
    return np.frombuffer(payload, dtype="<f4", count=dim)


def l2_distance(a, b):
    """Euclidean distance between two float32 vectors, computed in float64."""
    diff = np.asarray(a, dtype=np.float64) - np.asarray(b, dtype=np.float64)
    value = math.sqrt(float(np.dot(diff, diff)))
    if math.isnan(value) or math.isinf(value):
        raise DatabaseError("internal_error", "non-finite distance")
    return value


def chunk_l2_distances(matrix, query):
    """Euclidean distances between each row of ``matrix`` and ``query``."""
    diff = matrix.astype(np.float64) - np.asarray(query, dtype=np.float64)
    return np.sqrt(np.einsum("ij,ij->i", diff, diff))
