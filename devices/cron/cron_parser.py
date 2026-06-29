"""Cron expression parser for the CortexLink Cron virtual device.

Replicates the C++ cron_parser.cpp logic in pure Python.
Supports standard 5-field cron expressions with UTC+8 timezone.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone

logger = logging.getLogger(__name__)

# UTC+8 timezone for consistency with the C++ RuleEngine and docs.
_UTC8 = timezone(timedelta(hours=8))


# =============================================================================
# Data structures
# =============================================================================


@dataclass
class CronExpr:
    """Parsed representation of a 5-field cron expression.

    Each set contains the matching values for that field.
    """

    minute: set[int] = field(default_factory=set)         # 0..59
    hour: set[int] = field(default_factory=set)           # 0..23
    day_of_month: set[int] = field(default_factory=set)   # 1..31
    month: set[int] = field(default_factory=set)          # 1..12
    day_of_week: set[int] = field(default_factory=set)    # 0..6 (0=Sunday)


# =============================================================================
# Internal helpers
# =============================================================================


def _parse_field(token: str, min_val: int, max_val: int) -> tuple[set[int] | None, str | None]:
    """Parse a single cron field token into a set of integer values.

    Supports: *, single values, ranges (1-5), steps (*/5, 1-10/2), lists (1,3,5).

    Args:
        token: The field string to parse.
        min_val: Minimum valid value for this field.
        max_val: Maximum valid value for this field.

    Returns:
        (values_set, None) on success, or (None, error_msg) on failure.
    """
    if not token:
        return None, "empty field"

    result: set[int] = set()

    # Split on comma for lists
    parts = token.split(",")

    for part in parts:
        # Check for step syntax: */N or N-M/N or N/N
        step = 1
        range_part = part

        if "/" in part:
            slash_pos = part.index("/")
            range_part = part[:slash_pos]
            step_str = part[slash_pos + 1:]

            try:
                step = int(step_str)
            except ValueError:
                return None, f"invalid step: {step_str}"

            if step <= 0:
                return None, f"invalid step: {step_str} (must be positive)"

        # Determine low/high for the range
        low = min_val
        high = max_val

        if range_part == "*":
            low = min_val
            high = max_val
        elif "-" in range_part:
            # Range: N-M
            dash_pos = range_part.index("-")
            lo_str = range_part[:dash_pos]
            hi_str = range_part[dash_pos + 1:]

            try:
                low = int(lo_str)
                high = int(hi_str)
            except ValueError:
                return None, f"invalid range: {range_part}"
        else:
            # Single value
            try:
                val = int(range_part)
            except ValueError:
                return None, f"invalid value: {range_part}"
            low = high = val

        # Bounds check
        if low < min_val or high > max_val or low > high:
            return None, (
                f"range {low}-{high} out of bounds [{min_val},{max_val}]"
            )

        # Fill with step
        for v in range(low, high + 1, step):
            result.add(v)

    return result, None


# =============================================================================
# Public API
# =============================================================================


def parse(expr: str) -> tuple[CronExpr | None, str | None]:
    """Parse a standard 5-field cron expression.

    Fields: minute hour day-of-month month day-of-week
    Ranges: 0-59  0-23  1-31         1-12   0-6 (0=Sunday)

    Supported syntax per field:
        *       wildcard
        5       single value
        1-5     inclusive range
        */5     every 5th value (step)
        1,3,5   comma-separated list
        1-5/2   range with step

    Args:
        expr: The cron expression string to parse.

    Returns:
        (CronExpr, None) on success, or (None, error_msg) on failure.
    """
    fields = expr.split()

    if len(fields) != 5:
        return None, f"expected 5 fields, got {len(fields)}"

    result = CronExpr()

    # Field 0: minute (0-59)
    values, err = _parse_field(fields[0], 0, 59)
    if err:
        return None, f"minute: {err}"
    result.minute = values

    # Field 1: hour (0-23)
    values, err = _parse_field(fields[1], 0, 23)
    if err:
        return None, f"hour: {err}"
    result.hour = values

    # Field 2: day-of-month (1-31)
    values, err = _parse_field(fields[2], 1, 31)
    if err:
        return None, f"day-of-month: {err}"
    result.day_of_month = values

    # Field 3: month (1-12)
    values, err = _parse_field(fields[3], 1, 12)
    if err:
        return None, f"month: {err}"
    result.month = values

    # Field 4: day-of-week (0-7; 7 aliases to 0 = Sunday)
    values, err = _parse_field(fields[4], 0, 7)
    if err:
        return None, f"day-of-week: {err}"

    for v in values:
        if v == 7:
            result.day_of_week.add(0)  # alias: 7 → 0 (Sunday)
        else:
            result.day_of_week.add(v)

    return result, None


def matches(parsed: CronExpr,
            minute: int, hour: int, day_of_month: int,
            month: int, day_of_week: int) -> bool:
    """Check whether the given wall-clock time matches a parsed cron expression.

    Args:
        parsed: A CronExpr returned by parse().
        minute: 0-59
        hour: 0-23
        day_of_month: 1-31
        month: 1-12
        day_of_week: 0-6 (0=Sunday)

    Returns:
        True if the time matches the expression.
    """
    if minute not in parsed.minute:
        return False
    if hour not in parsed.hour:
        return False
    if day_of_month not in parsed.day_of_month:
        return False
    if month not in parsed.month:
        return False
    if day_of_week not in parsed.day_of_week:
        return False
    return True


def get_current_time_utc8() -> tuple[int, int, int, int, int]:
    """Get current wall-clock time in UTC+8.

    Returns:
        (minute, hour, day, month, day_of_week)
        - minute: 0-59
        - hour: 0-23
        - day: 1-31
        - month: 1-12
        - day_of_week: 0-6 (0=Sunday)
    """
    now = datetime.now(_UTC8)
    # Python weekday(): Monday=0 ... Sunday=6
    # We need Sunday=0 ... Saturday=6
    dow = (now.weekday() + 1) % 7  # Sunday=0
    return (now.minute, now.hour, now.day, now.month, dow)


def parse_offset(offset_str: str) -> tuple[int | None, str | None]:
    """Parse a human-readable time offset string into total seconds.

    Supported units:
        s = seconds   m = minutes   h = hours   d = days
    Examples:
        "30m"  "2h"  "1d"  "1h30m"  "2d6h"  "90s"

    Units may appear in any order and may be repeated.

    Args:
        offset_str: The offset string to parse.

    Returns:
        (total_seconds, None) on success, or (None, error_msg) on failure.
    """
    if not offset_str:
        return None, "offset string is empty"

    total_seconds = 0
    num_buf = ""

    for ch in offset_str:
        if ch.isdigit():
            num_buf += ch
        elif ch in ("s", "m", "h", "d"):
            if not num_buf:
                return None, f"missing number before unit '{ch}'"

            try:
                val = int(num_buf)
            except ValueError:
                return None, f"invalid number: {num_buf}"

            num_buf = ""

            if ch == "s":
                total_seconds += val
            elif ch == "m":
                total_seconds += val * 60
            elif ch == "h":
                total_seconds += val * 3600
            elif ch == "d":
                total_seconds += val * 86400
        else:
            return None, f"unknown unit: '{ch}'"

    if num_buf:
        return None, f"trailing number without unit: {num_buf}"

    if total_seconds <= 0:
        return None, "offset must be positive"

    return total_seconds, None


def make_cron_from_offset(offset_seconds: int) -> str:
    """Compute an absolute cron expression that fires once at now + offset (UTC+8).

    The resulting expression pins minute, hour, day-of-month, and month.
    Day-of-week is set to * (wildcard).

    Args:
        offset_seconds: Seconds from now to fire.

    Returns:
        A 5-field cron expression string.
    """
    target = datetime.now(_UTC8) + timedelta(seconds=offset_seconds)
    return f"{target.minute} {target.hour} {target.day} {target.month} *"


# =============================================================================
# Self-test (run with: python3 cron_parser.py)
# =============================================================================

if __name__ == "__main__":
    import sys

    passed = 0
    failed = 0

    def test(name: str, condition: bool, detail: str = "") -> None:
        global passed, failed
        if condition:
            print(f"  PASS: {name}")
            passed += 1
        else:
            print(f"  FAIL: {name} — {detail}")
            failed += 1

    print("=== Cron Parser Unit Tests ===\n")

    # --- parse() tests ---
    print("--- parse() ---")

    # Every minute
    ce, err = parse("* * * * *")
    test("* * * * * parses", ce is not None, str(err))
    if ce:
        test("  minute has all 60", len(ce.minute) == 60)
        test("  hour has all 24", len(ce.hour) == 24)
        test("  dom has all 31", len(ce.day_of_month) == 31)
        test("  month has all 12", len(ce.month) == 12)
        test("  dow has all 7", len(ce.day_of_week) == 7)

    # Specific value
    ce, err = parse("0 7 * * *")
    test("0 7 * * * parses", ce is not None, str(err))
    if ce:
        test("  minute = {0}", ce.minute == {0})
        test("  hour = {7}", ce.hour == {7})

    # Range
    ce, err = parse("0 9-17 * * *")
    test("0 9-17 * * * parses", ce is not None, str(err))
    if ce:
        test("  hour has 9 values", len(ce.hour) == 9)

    # Step
    ce, err = parse("*/5 * * * *")
    test("*/5 * * * * parses", ce is not None, str(err))
    if ce:
        test("  minute has 12 values", len(ce.minute) == 12)
        test("  minute includes 0,5,10", {0, 5, 10}.issubset(ce.minute))

    # List
    ce, err = parse("0 7,12,18 * * *")
    test("0 7,12,18 * * * parses", ce is not None, str(err))
    if ce:
        test("  hour = {7,12,18}", ce.hour == {7, 12, 18})

    # Range with step
    ce, err = parse("0 9-17/2 * * 1-5")
    test("0 9-17/2 * * 1-5 parses", ce is not None, str(err))
    if ce:
        test("  hour = {9,11,13,15,17}", ce.hour == {9, 11, 13, 15, 17})
        test("  dow = {1,2,3,4,5}", ce.day_of_week == {1, 2, 3, 4, 5})

    # Day-of-week alias: 7 → 0
    ce, err = parse("* * * * 0,7")
    test("* * * * 0,7 parses", ce is not None, str(err))
    if ce:
        test("  dow has only 0 (7→0)", ce.day_of_week == {0})

    # Error: wrong number of fields
    ce, err = parse("* * * *")
    test("* * * * fails (4 fields)", ce is None, str(err))

    # Error: invalid value
    ce, err = parse("60 * * * *")
    test("60 * * * * fails (minute 60 out of range)", ce is None, str(err))

    # Error: invalid step
    ce, err = parse("*/0 * * * *")
    test("*/0 * * * * fails (step 0)", ce is None, str(err))

    # --- matches() tests ---
    print("\n--- matches() ---")

    ce, _ = parse("0 7 * * *")
    test("0 7 * * * matches (0,7,15,6,2)", matches(ce, 0, 7, 15, 6, 2))
    test("0 7 * * * NOT match (1,7,15,6,2)", not matches(ce, 1, 7, 15, 6, 2))
    test("0 7 * * * NOT match (0,8,15,6,2)", not matches(ce, 0, 8, 15, 6, 2))

    # --- get_current_time_utc8() test ---
    print("\n--- get_current_time_utc8() ---")
    minute, hour, day, month, dow = get_current_time_utc8()
    test("minute in 0-59", 0 <= minute <= 59, f"got {minute}")
    test("hour in 0-23", 0 <= hour <= 23, f"got {hour}")
    test("day in 1-31", 1 <= day <= 31, f"got {day}")
    test("month in 1-12", 1 <= month <= 12, f"got {month}")
    test("dow in 0-6", 0 <= dow <= 6, f"got {dow}")
    print(f"    Current UTC+8: {hour:02d}:{minute:02d}, {month}/{day}, dow={dow}")

    # --- parse_offset() tests ---
    print("\n--- parse_offset() ---")

    tests = [
        ("30m", 30 * 60),
        ("2h", 2 * 3600),
        ("1d", 86400),
        ("1h30m", 3600 + 1800),
        ("2d6h", 2 * 86400 + 6 * 3600),
        ("90s", 90),
        ("1d12h30m15s", 86400 + 12 * 3600 + 30 * 60 + 15),
    ]
    for offset_str, expected in tests:
        secs, err = parse_offset(offset_str)
        test(f"parse_offset('{offset_str}') = {expected}s",
             secs == expected,
             f"got {secs}, err={err}")

    # Error cases
    secs, err = parse_offset("")
    test("parse_offset('') fails (empty)", secs is None, str(err))

    secs, err = parse_offset("abc")
    test("parse_offset('abc') fails (unknown unit)", secs is None, str(err))

    secs, err = parse_offset("m30")
    test("parse_offset('m30') fails (unit before number)", secs is None, str(err))

    # --- make_cron_from_offset() tests ---
    print("\n--- make_cron_from_offset() ---")
    expr = make_cron_from_offset(3600)  # 1 hour from now
    fields = expr.split()
    test("make_cron_from_offset(3600) has 5 fields", len(fields) == 5)
    test("  dow is *", fields[4] == "*")
    print(f"    1 hour from now → expr = '{expr}'")

    # --- Summary ---
    print(f"\n=== Results: {passed} passed, {failed} failed ===")
    sys.exit(0 if failed == 0 else 1)
