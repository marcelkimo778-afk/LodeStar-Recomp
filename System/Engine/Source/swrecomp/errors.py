"""Shared error types and diagnostics."""

from __future__ import annotations


class SwRecompError(Exception):
    """Base class for expected, user-reportable failures."""


class FormatError(SwRecompError):
    """Input data is malformed or inconsistent."""


class UnsupportedError(SwRecompError):
    """Input is well-formed but uses a feature this build does not support."""

    def __init__(self, message: str, detail: dict | None = None) -> None:
        super().__init__(message)
        self.detail = detail or {}


class MissingInputError(SwRecompError):
    """A required file or directory is absent."""
