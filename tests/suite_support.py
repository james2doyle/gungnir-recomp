"""Shared helpers for consolidated gates under unittest discovery."""

import sys


def loaded(name):
    return name in sys.modules or name.rsplit(".", 1)[-1] in sys.modules


def missing(names):
    return [name for name in names if not loaded(name)]
