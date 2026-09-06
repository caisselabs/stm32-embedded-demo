#!/usr/bin/env python3
#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Tests for tools/logmon.

The TUI itself is not covered here -- everything below the UI is, which is
where the decoding risk lives. Tests needing CIB's mipi_messages.py skip
themselves when the project has not been configured.

Run standalone:  python3 -m unittest discover -s tools/test -v
"""

import os
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools"))

from logmon import cib  # noqa: E402
from logmon.catalog import Catalog  # noqa: E402
from logmon.sources import FileSource  # noqa: E402

def _importable(name: str) -> bool:
    """Whether `import name` would actually succeed.

    An actual import, not importlib.util.find_spec: a module can be present on
    the path and still fail to import, and it is the import that the tests
    below depend on.
    """
    import importlib

    try:
        importlib.import_module(name)
        return True
    except ImportError:
        return False


# `import textual` is not the requirement -- logmon.tui is. Ubuntu 24.04 ships
# python3-textual 0.1.13, which imports happily and has none of the API the TUI
# is written against (textual.work, ComposeResult, DataTable), so a bare
# `_importable("textual")` says yes and then every TuiTest dies on ImportError.
HAVE_TEXTUAL = _importable("logmon.tui")
HAVE_PYSERIAL = _importable("serial")

HAVE_CIB = cib.find_cib_python() is not None


def short32(msg_id):
    """Encode a catalog ID the way the target does: (id << 4) | type."""
    return (msg_id << 4) | 1


def stream(ids):
    return struct.pack(f"<{len(ids)}I", *[short32(i) for i in ids])


CATALOG = {
    "messages": [
        {"id": i, "msg": name, "type": "msg", "arg_types": [],
         "args": [], "tags": []}
        for i, name in enumerate(["alpha", "bravo", "charlie", "delta"])
    ],
    "modules": [{"id": 0, "string": "app"}],
    "enums": {},
}
NAMES = ["alpha", "bravo", "charlie", "delta"]


def write_catalog(directory):
    import json

    path = Path(directory) / "catalog.json"
    path.write_text(json.dumps(CATALOG))
    return path


class CatalogTest(unittest.TestCase):
    def test_indexes_messages_and_modules(self):
        with tempfile.TemporaryDirectory() as d:
            c = Catalog.load(write_catalog(d))
            self.assertEqual(len(c), 4)
            self.assertEqual(c.messages[2]["msg"], "charlie")
            self.assertEqual(c.modules[0], "app")

    def test_reloads_when_the_file_changes(self):
        import json

        with tempfile.TemporaryDirectory() as d:
            path = write_catalog(d)
            c = Catalog.load(path)
            self.assertFalse(c.reload_if_changed())

            db = dict(CATALOG)
            db["messages"] = CATALOG["messages"] + [
                {"id": 4, "msg": "echo", "type": "msg", "arg_types": [],
                 "args": [], "tags": []}
            ]
            # mtime has 1s granularity on some filesystems; force a change.
            path.write_text(json.dumps(db))
            os.utime(path, (0, 0))

            self.assertTrue(c.reload_if_changed())
            self.assertEqual(len(c), 5)


class FileSourceTest(unittest.TestCase):
    def test_yields_all_bytes_in_chunks(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "cap.bin"
            data = stream(range(4)) * 10
            p.write_bytes(data)
            got = b"".join(FileSource(p, chunk=7))
            self.assertEqual(got, data)


@unittest.skipUnless(HAVE_CIB, "CIB mipi_messages.py not found -- configure first")
class DecoderTest(unittest.TestCase):
    def setUp(self):
        from logmon.decode import Decoder

        self.tmp = tempfile.TemporaryDirectory()
        self.catalog = Catalog.load(write_catalog(self.tmp.name))
        self.Decoder = Decoder
        self.decoder = Decoder(self.catalog)

    def tearDown(self):
        self.tmp.cleanup()

    def texts(self, records):
        return [r.text for r in records if r.ok]

    def test_decodes_a_whole_stream(self):
        out = self.decoder.feed(stream([0, 1, 2, 3]))
        self.assertEqual(self.texts(out), NAMES)
        self.assertEqual(self.decoder.stats.packets, 4)
        self.assertEqual(self.decoder.stats.errors, 0)

    def test_packets_split_across_chunks(self):
        """The serial case: reads land wherever they land."""
        data = stream([0, 1, 2, 3])
        got = []
        for i in range(0, len(data), 3):      # 3 bytes: never word-aligned
            got += self.texts(self.decoder.feed(data[i:i + 3]))
        self.assertEqual(got, NAMES)

    def test_one_byte_at_a_time(self):
        data = stream([0, 1, 2, 3])
        got = []
        for b in data:
            got += self.texts(self.decoder.feed(bytes([b])))
        self.assertEqual(got, NAMES)

    def test_partial_packet_is_held_not_dropped(self):
        self.assertEqual(self.decoder.feed(stream([0])[:3]), [])
        self.assertEqual(self.decoder.pending, 3)
        out = self.decoder.feed(stream([0])[3:])
        self.assertEqual(self.texts(out), ["alpha"])

    def test_resyncs_after_a_corrupt_word(self):
        data = stream([0]) + struct.pack("<I", 0x0000000F) + stream([1, 2])
        out = self.decoder.feed(data)
        self.assertEqual(self.texts(out), ["alpha", "bravo", "charlie"])
        self.assertEqual(self.decoder.stats.errors, 1)
        self.assertEqual(self.decoder.stats.resync_bytes, 4)
        self.assertTrue(any(not r.ok for r in out))

    def test_unknown_id_is_reported_not_fatal(self):
        """A stale catalog must not take the monitor down."""
        data = stream([0]) + struct.pack("<I", short32(99)) + stream([1])
        out = self.decoder.feed(data)
        self.assertEqual(self.texts(out), ["alpha", "bravo"])
        bad = [r for r in out if not r.ok]
        self.assertEqual(len(bad), 1)
        self.assertIn("99", bad[0].text)

    def test_trailing_unknown_id_does_not_stall_forever(self):
        """An undecodable tail must not buffer indefinitely."""
        from logmon.decode import MAX_PACKET_BYTES

        self.decoder.feed(struct.pack("<I", short32(99)))
        self.assertEqual(self.decoder.stats.packets, 0)
        # Once past the cap it is judged corrupt rather than incomplete.
        out = self.decoder.feed(b"\x00" * MAX_PACKET_BYTES)
        self.assertTrue(any(not r.ok for r in out))

    def test_never_emits_a_corrupt_packet_as_valid(self):
        """Random corruption may lose messages, never invent them."""
        import random

        rng = random.Random(20260831)
        for trial in range(50):
            with self.subTest(trial=trial):
                data = bytearray(stream([i % 4 for i in range(40)]))
                for _ in range(5):
                    data[rng.randrange(len(data))] = rng.randrange(256)
                d = self.Decoder(self.catalog)
                for r in d.feed(bytes(data)):
                    if r.ok:
                        self.assertIn(r.text, NAMES)

    def test_recovers_within_a_bounded_distance(self):
        """Corruption early on must not poison the rest of the stream."""
        data = bytearray(stream([i % 4 for i in range(40)]))
        data[5] = 0xFF
        out = self.decoder.feed(bytes(data))
        # Only the packet containing the damaged byte should be lost.
        self.assertGreaterEqual(len(self.texts(out)), 38)


@unittest.skipUnless(HAVE_CIB, "CIB mipi_messages.py not found -- configure first")
class AlignmentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.catalog = Catalog.load(write_catalog(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_detects_every_offset(self):
        from logmon.decode import detect_alignment

        body = stream([i % 4 for i in range(20)])
        for offset in range(4):
            with self.subTest(offset=offset):
                sample = b"\xff" * offset + body
                self.assertEqual(
                    detect_alignment(sample, self.catalog), offset
                )

    def test_aligned_stream_stays_at_zero(self):
        from logmon.decode import detect_alignment

        self.assertEqual(
            detect_alignment(stream([0, 1, 2, 3]), self.catalog), 0
        )


@unittest.skipUnless(HAVE_CIB, "CIB mipi_messages.py not found -- configure first")
@unittest.skipUnless(HAVE_PYSERIAL, "pyserial not installed")
class SerialPtyTest(unittest.TestCase):
    """Exercise the real pyserial path against a pty, with no hardware."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.catalog = Catalog.load(write_catalog(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_reads_and_decodes_from_a_pty(self):
        from logmon.decode import Decoder
        from logmon.sources import SerialSource

        master, slave = os.openpty()
        try:
            source = SerialSource(os.ttyname(slave), timeout=0.2)
            handle = source.open()
            os.write(master, stream([0, 1, 2, 3]))

            decoder = Decoder(self.catalog)
            texts = []
            for _ in range(20):                 # bounded: ~4s worst case
                texts += [
                    r.text for r in decoder.feed(handle.read(4096)) if r.ok
                ]
                if len(texts) >= 4:
                    break
            self.assertEqual(texts, NAMES)
        finally:
            source.close()
            os.close(master)
            os.close(slave)



@unittest.skipUnless(HAVE_CIB, "CIB mipi_messages.py not found -- configure first")
@unittest.skipUnless(HAVE_TEXTUAL, "logmon.tui needs a newer textual")
class TuiTest(unittest.IsolatedAsyncioTestCase):
    """Drive the Textual app headlessly with its own test pilot.

    These exist because the UI touches Textual APIs the decoder tests never
    reach -- DataTable.remove_row for the row cap, worker threads, the pause
    path -- and a Textual upgrade could break any of them silently.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.catalog = Catalog.load(write_catalog(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def capture(self, data):
        path = Path(self.tmp.name) / "cap.bin"
        path.write_bytes(data)
        return path

    def make_endless_app(self):
        """An app whose source never ends, the way a serial port never ends."""
        from logmon.sources import Source
        from logmon.tui import LogMonApp

        class EndlessSource(Source):
            """Endless for the length of the test, then self-terminating.

            A truly infinite source would make a regression *hang* the suite
            rather than fail it, and a hung CI job is worse than a red one.
            The deadline is far longer than the assertion below waits, so the
            test still fails on its own merits; it only guarantees the thread
            eventually dies so the process can exit and report.
            """

            name = "endless"
            DEADLINE = 8.0

            def __init__(self):
                self.closed = False
                self.reads = 0

            def __iter__(self):
                giveup = time.monotonic() + self.DEADLINE
                while time.monotonic() < giveup:
                    self.reads += 1
                    # Stand in for SerialSource's read timeout on a quiet link.
                    time.sleep(0.01)
                    yield b""

            def close(self):
                self.closed = True

        src = EndlessSource()
        return LogMonApp(src, self.catalog, 0), src

    async def test_quit_stops_the_reader_thread(self):
        """`q` must end the worker, not just tear down the UI.

        A thread worker cannot be interrupted, so an endless source keeps the
        thread -- and therefore the process -- alive after the TUI is gone.
        The worker has to poll for cancellation. Asserted by way of the source
        being closed, which only happens when the loop is left and the `with`
        unwinds.
        """
        app, src = self.make_endless_app()
        async with app.run_test() as pilot:
            for _ in range(50):
                await pilot.pause()
                if src.reads:
                    break
            self.assertTrue(src.reads, "reader thread never started")
            await pilot.press("q")

        # run_test() exiting means App._shutdown() ran and cancelled workers.
        for _ in range(200):
            if src.closed:
                break
            time.sleep(0.01)
        self.assertTrue(
            src.closed,
            "reader thread did not exit after quit: the source was never "
            "closed, so the `with` in read_source() never unwound",
        )

        leaked = [
            t for t in threading.enumerate()
            if t.is_alive() and not t.daemon and t is not threading.main_thread()
            and "asyncio" not in t.name.lower()
        ]
        self.assertEqual(
            [t.name for t in leaked], [],
            "non-daemon threads still running after quit; the process would hang",
        )

    def make_app(self, data, align=0, chunk=8):
        from logmon.sources import FileSource
        from logmon.tui import LogMonApp

        return LogMonApp(
            FileSource(self.capture(data), chunk=chunk, delay=0.002),
            self.catalog,
            align,
        )

    async def settle(self, app, want, tries=80):
        import asyncio

        from textual.widgets import DataTable

        table = app.query_one(DataTable)
        for _ in range(tries):
            await asyncio.sleep(0.02)
            if table.row_count >= want:
                break
        return table

    def messages(self, table):
        return [str(table.get_row_at(i)[3]) for i in range(table.row_count)]

    async def test_decodes_a_replay_into_rows(self):
        app = self.make_app(stream([0, 1, 2, 3]))
        async with app.run_test(size=(100, 24)):
            table = await self.settle(app, 4)
            self.assertEqual(self.messages(table), NAMES)

    async def test_shows_undecodable_words_rather_than_dying(self):
        data = stream([0]) + struct.pack("<I", 0xF) + stream([1])
        app = self.make_app(data, chunk=4)
        async with app.run_test(size=(100, 24)):
            table = await self.settle(app, 3)
            rows = self.messages(table)
            self.assertTrue(any("undecodable" in r for r in rows), rows)
            self.assertEqual(app.decoder.stats.errors, 1)

    async def test_row_cap_trims_oldest(self):
        """DataTable.remove_row is the API most likely to shift under us."""
        import asyncio

        from logmon import tui

        original, tui.MAX_ROWS = tui.MAX_ROWS, 10
        try:
            app = self.make_app(stream([i % 4 for i in range(50)]), chunk=16)
            async with app.run_test(size=(100, 24)):
                table = await self.settle(app, 10)
                for _ in range(30):
                    await asyncio.sleep(0.02)
                self.assertEqual(app.decoder.stats.packets, 50)
                self.assertEqual(table.row_count, 10)
                self.assertEqual(len(app._row_keys), 10)
        finally:
            tui.MAX_ROWS = original

    async def test_pause_holds_rows_then_drains(self):
        import asyncio

        app = self.make_app(stream([i % 4 for i in range(40)]), chunk=4)
        async with app.run_test(size=(100, 24)) as pilot:
            table = await self.settle(app, 2)
            await pilot.press("space")
            self.assertTrue(app.paused)
            frozen = table.row_count
            for _ in range(10):
                await asyncio.sleep(0.02)
            self.assertEqual(table.row_count, frozen, "rows leaked while paused")

            await pilot.press("space")
            self.assertFalse(app.paused)
            for _ in range(60):
                await asyncio.sleep(0.02)
                if table.row_count == app.decoder.stats.packets:
                    break
            # Everything decoded while paused must show up on resume, not be
            # dropped on the floor.
            self.assertEqual(table.row_count, app.decoder.stats.packets)
            self.assertEqual(len(app._pending), 0)

    async def test_auto_alignment_in_the_ui(self):
        body = stream([i % 4 for i in range(20)])
        app = self.make_app(b"\xff" * 3 + body, align="auto", chunk=64)
        async with app.run_test(size=(100, 24)):
            table = await self.settle(app, 10)
            self.assertEqual(app.align, 3)
            self.assertEqual(self.messages(table)[:4], NAMES)

    async def test_status_bar_markup_renders(self):
        """Markup must be interpreted, not printed literally."""
        from logmon.tui import StatusBar

        app = self.make_app(stream([0, 1]))
        async with app.run_test(size=(100, 24)):
            await self.settle(app, 2)
            svg = app.export_screenshot()
            self.assertIn("LIVE", svg)
            self.assertNotIn("[b green]", svg)
            self.assertIn("packets=", svg.replace("&#160;", " "))
            self.assertIsInstance(app.query_one(StatusBar), StatusBar)


class OptionalDependencyTest(unittest.TestCase):
    """Guard against the optional-dependency skips going unnoticed.

    TuiTest needs textual and SerialPtyTest needs pyserial; both skip
    themselves when the package is missing, which is right for a bare checkout
    and wrong for CI -- there the suite would shrink by seven tests and still
    report success. CI sets LOGMON_REQUIRE_OPTIONAL=1, which makes a missing
    package fail here instead.
    """

    @unittest.skipUnless(
        os.environ.get("LOGMON_REQUIRE_OPTIONAL") == "1",
        "set LOGMON_REQUIRE_OPTIONAL=1 to require the optional dependencies",
    )
    def test_optional_dependencies_are_installed(self):
        # Asserts on the same flags the skips read, so the guard cannot drift
        # away from the conditions it is guarding.
        missing = [
            name
            for name, available in (
                ("textual", HAVE_TEXTUAL),
                ("pyserial", HAVE_PYSERIAL),
            )
            if not available
        ]
        self.assertEqual(
            missing,
            [],
            f"missing {missing}: the TUI and serial tests would silently "
            "skip. Install `textual` from PyPI (the distro package can be "
            "far too old) and python3-serial.",
        )


if __name__ == "__main__":
    unittest.main()
