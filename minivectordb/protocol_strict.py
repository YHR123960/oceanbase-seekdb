"""Strict JSON parsing matching the judge's expectations.

Rejects NaN/Infinity constants and duplicate object keys so malformed
requests are surfaced instead of silently accepted.
"""

import json


def strict_json(text):
    def invalid(value):
        raise ValueError(f"non-finite JSON number: {value}")

    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError(f"duplicate JSON key: {key}")
            result[key] = value
        return result

    return json.loads(text, parse_constant=invalid, object_pairs_hook=pairs)
