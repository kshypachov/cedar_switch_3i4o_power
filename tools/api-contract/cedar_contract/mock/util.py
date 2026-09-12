# SPDX-License-Identifier: Apache-2.0
"""Small shared helpers. Nothing here makes a decision."""

from __future__ import annotations

import base64
from typing import Any

from ..errors import ApiError, detail_dict


def deep_copy(value: Any) -> Any:
    """Copy nested plain data.

    Snapshots go out to callers that may keep them, and a committed
    configuration that shares a dict with a candidate is a state machine with a
    hidden edge. `copy.deepcopy` would do it, but it also copies things this
    should never see, so keeping it to dict/list/scalar makes the contract of
    the function visible.
    """
    if isinstance(value, dict):
        return {k: deep_copy(v) for k, v in value.items()}
    if isinstance(value, list):
        return [deep_copy(v) for v in value]
    return value


def detail(err: ApiError) -> dict[str, object]:
    """An `ErrorDetail` for embedding in a job, transaction or interface."""
    return detail_dict(err)


def decode_ssid(value: str) -> bytes | None:
    """The exact SSID bytes, or None if the base64 is not valid.

    The contract carries an SSID as base64 precisely because it is 0-32 bytes
    that need not be UTF-8, so the decoded form is what any rule about it has to
    look at.
    """
    try:
        return base64.b64decode(value, validate=True)
    except (ValueError, TypeError):
        return None


def ssid_text(value: str) -> str:
    """A display string, with invalid bytes replaced rather than raising."""
    return (decode_ssid(value) or b"").decode("utf-8", "replace")
