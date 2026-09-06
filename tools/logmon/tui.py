#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Textual UI for the log monitor.

The reader runs in a worker thread and pushes records onto a queue; the UI
drains that queue on a timer and appends in batches. Decoding on the event loop
instead would let a chatty target starve the UI -- at 115200 the target can
produce a packet every ~350us, far faster than a sensible repaint rate.
"""

from __future__ import annotations

import queue
from collections import deque
from datetime import datetime

from rich.text import Text
from textual import work
from textual.app import App, ComposeResult
from textual.widgets import DataTable, Footer, Static
from textual.worker import get_current_worker

from .catalog import Catalog
from .cib import load_mipi_messages
from .decode import Aligner, Decoder

MAX_ROWS = 20000


def shorten(text: str, limit: int = 44) -> str:
    """Keep the tail, which is the part that identifies a path or port."""
    return text if len(text) <= limit else "..." + text[-(limit - 3):]


class StatusBar(Static):
    """Source, catalog and running counters.

    The source is truncated: a --replay path can be long enough to wrap the
    whole widget and push the counters line out of view entirely.
    """

    def update_status(self, *, source, catalog, stats, paused, align):
        state = "[b yellow]PAUSED[/]" if paused else "[b green]LIVE[/]"
        align_txt = "?" if align is None else str(align)
        self.update(
            f"{state}  [b]{shorten(source)}[/]  "
            f"catalog=[b]{catalog.path.name}[/] "
            f"({len(catalog)} msgs)  align={align_txt}\n"
            f"packets=[b]{stats.packets}[/]  errors=[b]{stats.errors}[/]  "
            f"discarded=[b]{stats.resync_bytes}[/]B  "
            f"in=[b]{stats.bytes_in}[/]B"
        )


class LogMonApp(App):
    TITLE = "logmon"

    CSS = """
    StatusBar { height: 3; padding: 0 1; background: $panel; }
    DataTable { height: 1fr; }
    """

    BINDINGS = [
        ("space", "toggle_pause", "Pause"),
        ("c", "clear", "Clear"),
        ("q", "quit", "Quit"),
    ]

    def __init__(self, source, catalog: Catalog, align, capture=None):
        super().__init__()
        self.source = source
        self.catalog = catalog
        self.align = None if align == "auto" else align
        self.capture = capture
        self._mipi = load_mipi_messages()
        self.decoder = Decoder(catalog, mipi=self._mipi)
        self.queue: queue.Queue = queue.Queue()
        # Records drained off the worker's queue but not yet shown. Pausing
        # holds rows here rather than dropping them, so a pause taken to read
        # something does not silently lose what arrived meanwhile. Bounded, so
        # a long pause on a chatty target cannot grow without limit -- and the
        # --capture file has everything regardless.
        self._pending: deque = deque(maxlen=MAX_ROWS)
        self.paused = False
        self._row_keys: list = []

    def compose(self) -> ComposeResult:
        yield StatusBar()
        yield DataTable(zebra_stripes=True)
        yield Footer()

    def on_mount(self) -> None:
        table = self.query_one(DataTable)
        table.add_columns("time", "sev", "module", "message")
        table.cursor_type = "row"
        self.set_interval(0.1, self.drain)
        self.set_interval(2.0, self.check_catalog)
        self.read_source()

    # -- reader ------------------------------------------------------------

    @work(thread=True, exclusive=True)
    def read_source(self) -> None:
        # A thread worker cannot be interrupted, only asked to stop: Textual's
        # App._shutdown() calls workers.cancel_all(), which sets this flag and
        # then waits. A live SerialSource iterates forever (a quiet link just
        # yields b""), so without this check the thread outlives the UI and the
        # process hangs after `q` with nothing on screen.
        #
        # SerialSource reads with a 0.1s timeout, so the flag is seen within
        # about that long. Leaving the loop lets the `with` close the port on
        # the thread that opened it, rather than closing it underneath a read
        # in flight from another thread.
        worker = get_current_worker()
        aligner = Aligner(self.catalog, self._mipi, align=self.align)
        with self.source as src:
            for chunk in src:
                if worker.is_cancelled:
                    return

                if self.capture and chunk:
                    self.capture.write(chunk)
                    self.capture.flush()

                data = aligner.feed(chunk)
                self.align = aligner.align
                for record in self.decoder.feed(data):
                    self.queue.put((datetime.now(), record))

            # A replay shorter than a full sample still has to land.
            tail = aligner.flush()
            self.align = aligner.align
            for record in self.decoder.feed(tail):
                self.queue.put((datetime.now(), record))

    # -- UI ----------------------------------------------------------------

    def drain(self) -> None:
        table = self.query_one(DataTable)

        # Always empty the queue, paused or not, so the reader thread is never
        # throttled by the UI.
        while True:
            try:
                self._pending.append(self.queue.get_nowait())
            except queue.Empty:
                break

        if self._pending and not self.paused:
            for stamp, record in self._pending:
                self._append(table, stamp, record)
            self._pending.clear()
            self._trim(table)
            table.scroll_end(animate=False)

        self.query_one(StatusBar).update_status(
            source=self.source.name,
            catalog=self.catalog,
            stats=self.decoder.stats,
            paused=self.paused,
            align=self.align,
        )

    def _append(self, table: DataTable, stamp, record) -> None:
        style = "red" if not record.ok else ""
        key = table.add_row(
            stamp.strftime("%H:%M:%S.%f")[:-3],
            Text(record.severity or "-", style=style),
            Text(record.module or "-", style=style),
            Text(record.text, style=style),
        )
        self._row_keys.append(key)

    def _trim(self, table: DataTable) -> None:
        while len(self._row_keys) > MAX_ROWS:
            table.remove_row(self._row_keys.pop(0))

    def check_catalog(self) -> None:
        """Pick up a rebuild without restarting the monitor."""
        if self.catalog.reload_if_changed():
            self.notify(f"catalog reloaded: {len(self.catalog)} messages")

    # -- actions -----------------------------------------------------------

    def action_toggle_pause(self) -> None:
        self.paused = not self.paused

    def action_clear(self) -> None:
        self.query_one(DataTable).clear()
        self._row_keys.clear()
        self._pending.clear()


def run_tui(args) -> int:
    from .cli import initial_alignment, make_source, resolve_catalog

    catalog = resolve_catalog(args)
    source = make_source(args)
    capture = open(args.capture, "wb") if args.capture else None
    try:
        LogMonApp(source, catalog, initial_alignment(args), capture).run()
    finally:
        if capture:
            capture.close()
    return 0
