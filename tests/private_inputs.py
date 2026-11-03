"""T13d — fail-closed private-input helper.

Local runs skip when the private disc (or its derived corpus) is absent.
CI sets GUNGNIR_REQUIRE_PRIVATE_INPUT=1, which turns the same skip into a
hard failure so a missing input can never look like a pass.
"""
import os
import unittest

STRICT_ENV = "GUNGNIR_REQUIRE_PRIVATE_INPUT"


def is_strict():
    return os.environ.get(STRICT_ENV) == "1"


def require_private_input(reason):
    """Skip non-strict, fail closed under strict mode."""
    if is_strict():
        raise AssertionError(f"private_input_required: {reason}")
    raise unittest.SkipTest(f"private_input_missing: {reason}")


def have_private_input(*paths):
    return all(path.is_file() for path in paths)
