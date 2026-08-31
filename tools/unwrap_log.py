#!/usr/bin/env python3
#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Flatten a dump of app_log::buffer into a stream log_decode.py can read.

The on-target log destination (src/logging/log_config.hpp) is a circular
buffer of 32-bit words preceded by a small header:

    magic       'CIBL', so a dump of the wrong symbol is caught here
    capacity    number of words in the ring
    total_words monotonic count of words ever written
    words[]     the ring itself

Feeding that straight to log_decode.py does not work, for two reasons:

1. The decoder reads packets back-to-back with no framing and no end marker.
   A dump of the whole ring hands it the unwritten tail, and the first zero
   word aborts it with "Unknown message type: 0".
2. Once total_words exceeds capacity the ring has rolled over, so the oldest
   surviving word is at index (total_words % capacity), not 0. A straight dump
   decodes without error but prints the log with a seam in the middle.

This script uses the header to emit exactly the live words, oldest first, and
reports on stderr how many were overwritten before the dump was taken.

Usage:
    (gdb) dump binary value log.bin app_log::buffer

    python3 tools/unwrap_log.py --input log.bin --output ordered.bin
    python3 <cib>/python/cib/log_decode.py \
        --input ordered.bin --json build/log_strings.json
"""

import argparse
import struct
import sys

# 'C' 'I' 'B' 'L' -- must match app_log::buffer_t::magic.
MAGIC = 0x4342494C
HEADER_FORMAT = "<3I"
HEADER_SIZE = struct.calcsize(HEADER_FORMAT)


def unwrap(raw):
    """Return (live words oldest-first, count lost to overwrite)."""
    if len(raw) < HEADER_SIZE:
        raise ValueError(f"{len(raw)} bytes is too short to hold a header")

    magic, capacity, total_words = struct.unpack_from(HEADER_FORMAT, raw, 0)
    if magic != MAGIC:
        raise ValueError(
            f"bad magic {magic:#010x} (expected {MAGIC:#010x}) -- dump "
            "app_log::buffer, not app_log::buffer.words"
        )

    expected = HEADER_SIZE + 4 * capacity
    if len(raw) < expected:
        raise ValueError(
            f"header declares {capacity} words ({expected} bytes) but the "
            f"dump is {len(raw)} bytes -- truncated?"
        )

    words = list(struct.unpack_from(f"<{capacity}I", raw, HEADER_SIZE))
    if total_words <= capacity:
        return words[:total_words], 0

    # Wrapped: the next write would land on the oldest retained word.
    start = total_words % capacity
    return words[start:] + words[:start], total_words - capacity


def main():
    parser = argparse.ArgumentParser(
        description="Flatten an app_log::buffer dump for log_decode.py."
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Input filename: gdb dump of app_log::buffer.",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output filename: packet stream for log_decode.py.",
    )
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        raw = f.read()

    try:
        words, lost = unwrap(raw)
    except ValueError as e:
        sys.exit(f"{args.input}: {e}")

    with open(args.output, "wb") as f:
        f.write(struct.pack(f"<{len(words)}I", *words))

    note = f", {lost} overwritten before the dump" if lost else ""
    print(f"{len(words)} words written to {args.output}{note}", file=sys.stderr)


if __name__ == "__main__":
    main()
