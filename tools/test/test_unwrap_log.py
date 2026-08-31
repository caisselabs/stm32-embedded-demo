#!/usr/bin/env python3
#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Tests for tools/unwrap_log.py.

Run from the repository root:

    python3 -m unittest discover -s tools/test -v

Stdlib unittest is used deliberately: this has to run anywhere the firmware
build runs, without a Python environment to set up first.

The tests in TestAgainstDecoder need CIB's log_decode.py, which only exists
once the project has been configured. They skip themselves when it is absent
so the rest of the suite still runs on a clean checkout.

Set BUILD_DIR to point at a build tree other than ./build, or CIB_LOG_DECODE
straight at a log_decode.py, if neither is where these tests look.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "unwrap_log.py"
def find_build_dir():
    """The configured build tree these tests read collateral out of."""
    override = os.environ.get("BUILD_DIR")
    if override:
        return Path(override)
    for name in ("build", "build-host"):
        candidate = REPO / name
        if (candidate / "CMakeCache.txt").exists():
            return candidate
    return REPO / "build"


BUILD = find_build_dir()
BUILD_CATALOG = BUILD / "log_strings.json"


def find_decoder():
    """Locate CIB's log_decode.py in the configured build.

    Where CPM unpacks a package depends on whether CPM_SOURCE_CACHE is set:
    without it the sources land in build/_deps/<name>-src, with it (as in CI)
    they live in the shared cache and _deps holds only the build trees. CMake
    records the resolved path either way, so read it rather than guessing.
    """
    override = os.environ.get("CIB_LOG_DECODE")
    if override:
        return Path(override) if Path(override).exists() else None

    suffix = Path("python") / "cib" / "log_decode.py"

    cache = BUILD / "CMakeCache.txt"
    if cache.exists():
        key = "CPM_PACKAGE_compile-time-init-build_SOURCE_DIR:INTERNAL="
        for line in cache.read_text().splitlines():
            if line.startswith(key):
                candidate = Path(line[len(key):].strip()) / suffix
                if candidate.exists():
                    return candidate

    candidate = BUILD / "_deps" / "compile-time-init-build-src" / suffix
    return candidate if candidate.exists() else None


DECODER = find_decoder()

sys.path.insert(0, str(REPO / "tools"))
import unwrap_log  # noqa: E402


# --------------------------------------------------------------------------
# Helpers modelling the on-target buffer.


def simulate_ring(packets, capacity):
    """Model app_log::log_destination::operator() writing `packets` words.

    Mirrors the C++ exactly:

        buffer.words[buffer.total_words % capacity] = word;
        ++buffer.total_words;

    Returns the (ring contents, total_words) a debugger would see.
    """
    words = [0] * capacity
    total = 0
    for w in packets:
        words[total % capacity] = w
        total += 1
    return words, total


def expected_live(packets, capacity):
    """What unwrap() must recover: the surviving words, oldest first."""
    live = packets[-capacity:] if capacity else []
    lost = max(0, len(packets) - capacity)
    return live, lost


def pack_dump(words, total, capacity=None, magic=unwrap_log.MAGIC):
    """Bytes equivalent to `dump binary value log.bin app_log::buffer`."""
    if capacity is None:
        capacity = len(words)
    return struct.pack("<3I", magic, capacity, total) + struct.pack(
        f"<{len(words)}I", *words
    )


def short32(msg_id):
    """Encode a catalog ID as a MIPI Sys-T Short32 packet word."""
    return (msg_id << 4) | 1


# --------------------------------------------------------------------------


class TestUnwrap(unittest.TestCase):
    """Direct tests of the unwrap() function."""

    def test_empty_buffer(self):
        words, lost = unwrap_log.unwrap(pack_dump([0] * 8, total=0))
        self.assertEqual(words, [])
        self.assertEqual(lost, 0)

    def test_partially_filled(self):
        ring = [11, 22, 33] + [0] * 5
        words, lost = unwrap_log.unwrap(pack_dump(ring, total=3))
        self.assertEqual(words, [11, 22, 33])
        self.assertEqual(lost, 0)

    def test_trailing_zeros_are_dropped(self):
        """The reason the tool exists: unwritten words must not be emitted."""
        ring = [11, 22] + [0] * 254
        words, _ = unwrap_log.unwrap(pack_dump(ring, total=2))
        self.assertEqual(words, [11, 22])
        self.assertNotIn(0, words)

    def test_exactly_full_is_not_treated_as_wrapped(self):
        """Boundary: total_words == capacity still starts at index 0."""
        ring = list(range(1, 9))
        words, lost = unwrap_log.unwrap(pack_dump(ring, total=8))
        self.assertEqual(words, ring)
        self.assertEqual(lost, 0)

    def test_wrapped_by_one(self):
        # 9 writes into an 8-word ring: word 1 landed on top of word 0.
        packets = list(range(1, 10))
        ring, total = simulate_ring(packets, capacity=8)
        words, lost = unwrap_log.unwrap(pack_dump(ring, total))
        self.assertEqual(words, packets[1:])
        self.assertEqual(lost, 1)

    def test_wrapped_by_a_whole_lap(self):
        """total_words % capacity == 0 while still wrapped: start index 0."""
        packets = list(range(1, 17))
        ring, total = simulate_ring(packets, capacity=8)
        words, lost = unwrap_log.unwrap(pack_dump(ring, total))
        self.assertEqual(words, packets[8:])
        self.assertEqual(lost, 8)

    def test_capacity_is_read_from_the_header(self):
        """Guards against a hardcoded 256."""
        ring = [7, 8, 9, 10]
        words, _ = unwrap_log.unwrap(pack_dump(ring, total=4, capacity=4))
        self.assertEqual(words, ring)

    def test_little_endian_words(self):
        """Byte order must match the Cortex-M4 and the decoder."""
        raw = struct.pack("<3I", unwrap_log.MAGIC, 1, 1) + b"\x11\x00\x00\x00"
        words, _ = unwrap_log.unwrap(raw)
        self.assertEqual(words, [0x11])

    def test_bad_magic_names_the_likely_mistake(self):
        raw = pack_dump([1, 17], total=2, magic=0x00000001)
        with self.assertRaises(ValueError) as ctx:
            unwrap_log.unwrap(raw)
        self.assertIn("app_log::buffer.words", str(ctx.exception))

    def test_truncated_payload(self):
        raw = pack_dump([0] * 256, total=2)[:40]
        with self.assertRaises(ValueError) as ctx:
            unwrap_log.unwrap(raw)
        self.assertIn("truncated", str(ctx.exception))

    def test_shorter_than_a_header(self):
        with self.assertRaises(ValueError) as ctx:
            unwrap_log.unwrap(b"\x00\x00\x00")
        self.assertIn("too short", str(ctx.exception))


class TestRingModel(unittest.TestCase):
    """unwrap() against an independent model of the ring, across many sizes."""

    def test_matches_the_model_everywhere(self):
        for capacity in (1, 2, 3, 7, 8, 16, 256):
            # Straddle every interesting point: empty, partial, exactly full,
            # one past full, and several laps around.
            for total in range(0, 3 * capacity + 2):
                with self.subTest(capacity=capacity, total=total):
                    packets = [short32(i % 8) for i in range(total)]
                    ring, written = simulate_ring(packets, capacity)
                    self.assertEqual(written, total)

                    words, lost = unwrap_log.unwrap(pack_dump(ring, written))
                    exp_words, exp_lost = expected_live(packets, capacity)
                    self.assertEqual(words, exp_words)
                    self.assertEqual(lost, exp_lost)

    def test_live_words_are_always_the_most_recent(self):
        """No matter the rotation, the last word out is the last word in."""
        capacity = 8
        for total in range(1, 40):
            with self.subTest(total=total):
                packets = list(range(1, total + 1))
                ring, written = simulate_ring(packets, capacity)
                words, _ = unwrap_log.unwrap(pack_dump(ring, written))
                self.assertEqual(words[-1], packets[-1])
                self.assertEqual(len(words), min(total, capacity))


class TestCli(unittest.TestCase):
    """The command-line behaviour the documented pipeline depends on."""

    def run_tool(self, raw, *args):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "log.bin"
            src.write_bytes(raw)
            return subprocess.run(
                [sys.executable, str(TOOL), "--input", str(src), *args],
                capture_output=True,
            )

    def test_stdout_is_a_clean_packet_stream(self):
        """Nothing but packets on stdout, or the pipe to log_decode breaks."""
        raw = pack_dump([1, 17] + [0] * 254, total=2)
        r = self.run_tool(raw)
        self.assertEqual(r.returncode, 0)
        self.assertEqual(r.stdout, struct.pack("<2I", 1, 17))

    def test_progress_goes_to_stderr(self):
        raw = pack_dump([1, 17] + [0] * 254, total=2)
        r = self.run_tool(raw)
        self.assertIn(b"2 words written", r.stderr)

    def test_overwrite_count_reported_only_when_lost(self):
        ring, total = simulate_ring(list(range(1, 301)), capacity=256)
        r = self.run_tool(pack_dump(ring, total))
        self.assertIn(b"44 overwritten", r.stderr)

        r = self.run_tool(pack_dump([1, 17] + [0] * 254, total=2))
        self.assertNotIn(b"overwritten", r.stderr)

    def test_output_to_file(self):
        raw = pack_dump([1, 17] + [0] * 254, total=2)
        with tempfile.TemporaryDirectory() as d:
            out = Path(d) / "ordered.bin"
            r = self.run_tool(raw, "--output", str(out))
            self.assertEqual(r.returncode, 0)
            self.assertEqual(out.read_bytes(), struct.pack("<2I", 1, 17))
            self.assertEqual(r.stdout, b"", "file mode must not write stdout")

    def test_bad_input_exits_nonzero_with_a_diagnostic(self):
        raw = pack_dump([1, 17] + [0] * 254, total=2, magic=1)
        r = self.run_tool(raw)
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"bad magic", r.stderr)
        self.assertEqual(r.stdout, b"")

    def test_missing_input_argument(self):
        r = subprocess.run(
            [sys.executable, str(TOOL)], capture_output=True
        )
        self.assertNotEqual(r.returncode, 0)


@unittest.skipUnless(
    DECODER is not None,
    "log_decode.py not found -- configure the project first",
)
class TestAgainstDecoder(unittest.TestCase):
    """End-to-end through CIB's real decoder.

    These pin the two failure modes that motivated the tool, by showing that
    the raw ring misbehaves and the unwrapped stream does not.
    """

    CATALOG = {
        "messages": [
            {"id": 0, "msg": "alpha", "type": "msg", "arg_types": [],
             "args": [], "tags": []},
            {"id": 1, "msg": "bravo", "type": "msg", "arg_types": [],
             "args": [], "tags": []},
            {"id": 2, "msg": "charlie", "type": "msg", "arg_types": [],
             "args": [], "tags": []},
            {"id": 3, "msg": "delta", "type": "msg", "arg_types": [],
             "args": [], "tags": []},
        ],
        "modules": [],
        "enums": {},
    }
    NAMES = ["alpha", "bravo", "charlie", "delta"]

    def decode(self, stream, catalog=None):
        """Run log_decode.py over a raw packet stream."""
        with tempfile.TemporaryDirectory() as d:
            data = Path(d) / "packets.bin"
            data.write_bytes(stream)
            cat = Path(d) / "catalog.json"
            cat.write_text(json.dumps(catalog or self.CATALOG))
            return subprocess.run(
                [sys.executable, str(DECODER), "--input", str(data),
                 "--json", str(cat)],
                capture_output=True,
                text=True,
            )

    def unwrap_bytes(self, raw):
        with tempfile.TemporaryDirectory() as d:
            src = Path(d) / "log.bin"
            src.write_bytes(raw)
            r = subprocess.run(
                [sys.executable, str(TOOL), "--input", str(src)],
                capture_output=True,
            )
            self.assertEqual(r.returncode, 0, r.stderr)
            return r.stdout

    def test_raw_ring_aborts_the_decoder(self):
        """Failure mode 1: trailing zeros decode as message type 0."""
        ring = [short32(0), short32(1)] + [0] * 254
        r = self.decode(struct.pack("<256I", *ring))
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("Unknown message type: 0", r.stderr)

    def test_unwrapped_stream_decodes_cleanly(self):
        ring = [short32(0), short32(1)] + [0] * 254
        stream = self.unwrap_bytes(pack_dump(ring, total=2))
        r = self.decode(stream)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.split(), ["alpha", "bravo"])

    def test_raw_wrapped_ring_decodes_out_of_order(self):
        """Failure mode 2: no error, just a seam. The dangerous one."""
        packets = [short32(i % 4) for i in range(10)]
        ring, _ = simulate_ring(packets, capacity=8)
        r = self.decode(struct.pack("<8I", *ring))
        self.assertEqual(r.returncode, 0, "wrapped ring decodes without error")
        expected_in_order = [self.NAMES[i % 4] for i in range(2, 10)]
        self.assertNotEqual(r.stdout.split(), expected_in_order)

    def test_unwrapped_wrapped_ring_is_in_order(self):
        packets = [short32(i % 4) for i in range(10)]
        ring, total = simulate_ring(packets, capacity=8)
        stream = self.unwrap_bytes(pack_dump(ring, total))
        r = self.decode(stream)
        self.assertEqual(r.returncode, 0, r.stderr)
        expected_in_order = [self.NAMES[i % 4] for i in range(2, 10)]
        self.assertEqual(r.stdout.split(), expected_in_order)

    def test_empty_buffer_decodes_to_nothing(self):
        stream = self.unwrap_bytes(pack_dump([0] * 8, total=0))
        r = self.decode(stream)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "")

    @unittest.skipUnless(BUILD_CATALOG.exists(), "log_strings.json not generated")
    def test_real_catalog_boot_sequence(self):
        """The words actually observed on target: {1, 17} with total_words 2.

        Also pins the Short32 layout the docs describe, (id << 4) | 1.
        """
        catalog = json.loads(BUILD_CATALOG.read_text())
        by_id = {m["id"]: m["msg"] for m in catalog["messages"]}
        if by_id.get(0) != "Booting" or by_id.get(1) != "Entering main loop":
            self.skipTest("catalog IDs have shifted since this was written")

        self.assertEqual(short32(0), 1)
        self.assertEqual(short32(1), 17)

        ring = [1, 17] + [0] * 254
        stream = self.unwrap_bytes(pack_dump(ring, total=2))
        r = self.decode(stream, catalog=catalog)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(
            r.stdout.splitlines(), ["Booting", "Entering main loop"]
        )


if __name__ == "__main__":
    unittest.main()
