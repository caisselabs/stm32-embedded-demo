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

Draining the queue is not enough on its own, though. Two things have to hold
for a constant stream not to wedge the UI, and both are about bounding work
rather than keeping up:

  * Bounded work per tick. The target can decode faster than any table can
    render, so the number of rows added per drain is capped and the overflow
    is dropped -- newest kept, oldest discarded, with a running count in the
    status bar. A live tail that silently falls further and further behind is
    worse than one that admits a gap, and --capture keeps the whole stream for
    a later --replay regardless.

  * Bounded cost per row removed. DataTable.remove_row is O(rows): it rebuilds
    the row-index map and invalidates the ordered-row cache, so shedding k rows
    off a table of n costs O(n*k). Trimming a full table one row at a time is
    what pinned the CPU at 100% and froze the UI. Rows are shed by rebuilding
    instead -- see _enforce_cap.
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

# Rows kept on screen. Sized by what a rebuild costs, not by what a scrollback
# might ideally hold: clear() + add_rows() runs about 50us a row, and since a
# rebuild is the one unavoidable hitch, MAX_ROWS is what sets how big that
# hitch is. 2000 rows measures at ~90ms worst case, which reads as responsive;
# 5000 is ~250ms, which does not. The whole stream is still in --capture.
MAX_ROWS = 2000

# How far the table may overshoot MAX_ROWS before it is rebuilt. Pure
# hysteresis: a rebuild costs the same whether it sheds one row or two
# thousand, so shedding them in one go is what amortizes it away. Keeping this
# equal to MAX_ROWS puts the amortized cost at roughly one rebuild per row-cap
# worth of traffic, about 50us per row rendered.
TRIM_SLACK = 2000

# Rows rendered per drain tick. A saturated 115200 link is 11520 bytes/s, so
# 2880 four-byte packets/s -- 288 per 0.1s tick. 400 clears that with headroom,
# so the intended link is rendered without dropping anything, while still
# bounding what one tick can cost when something faster (a replay streaming at
# disk speed) is on the other end.
MAX_APPEND_PER_TICK = 400


def shorten(text: str, limit: int = 44) -> str:
    """Keep the tail, which is the part that identifies a path or port."""
    return text if len(text) <= limit else "..." + text[-(limit - 3):]


class StatusBar(Static):
    """Source, catalog and running counters.

    The source is truncated: a --replay path can be long enough to wrap the
    whole widget and push the counters line out of view entirely.
    """

    def update_status(self, *, source, catalog, stats, paused, align,
                      dropped=0):
        state = "[b yellow]PAUSED[/]" if paused else "[b green]LIVE[/]"
        align_txt = "?" if align is None else str(align)
        # Only shown once it is non-zero: on a link the UI keeps up with, a
        # permanent "dropped=0" is noise, and when it is not zero it is the
        # thing the reader most needs to know about what is on screen.
        drop_txt = f"  [b yellow]dropped=[b]{dropped}[/][/]" if dropped else ""
        self.update(
            f"{state}  [b]{shorten(source)}[/]  "
            f"catalog=[b]{catalog.path.name}[/] "
            f"({len(catalog)} msgs)  align={align_txt}\n"
            f"packets=[b]{stats.packets}[/]  errors=[b]{stats.errors}[/]  "
            f"discarded=[b]{stats.resync_bytes}[/]B  "
            f"in=[b]{stats.bytes_in}[/]B{drop_txt}"
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
        # The rows currently on screen, as ready-to-add cell tuples. This is
        # the model a rebuild renders from, which is why it is capped at
        # MAX_ROWS while the table itself is allowed to run up to
        # MAX_ROWS + TRIM_SLACK.
        self._rows: deque = deque(maxlen=MAX_ROWS)
        # Records decoded but never shown, because they arrived faster than
        # the table could render them.
        self.dropped = 0

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
                self._stash(self.queue.get_nowait())
            except queue.Empty:
                break

        if self._pending and not self.paused:
            self._render(table)

        self.query_one(StatusBar).update_status(
            source=self.source.name,
            catalog=self.catalog,
            stats=self.decoder.stats,
            paused=self.paused,
            align=self.align,
            dropped=self.dropped,
        )

    def _stash(self, item) -> None:
        """Hold one record for the next render, counting what falls off.

        _pending is bounded, and a bounded deque discards silently. Counting
        here is what keeps the status bar honest about a view that has gaps.
        """
        if len(self._pending) == self._pending.maxlen:
            self.dropped += 1
        self._pending.append(item)

    def _render(self, table: DataTable) -> None:
        # Cap the work this tick. A target in a tight trace loop decodes far
        # more records per tick than any table can render in one, so keep the
        # newest MAX_APPEND_PER_TICK and count the rest as dropped. Rendering
        # oldest-first instead would leave the view lagging further behind
        # real time with every tick, which is the worse failure: a gap is
        # visible, a growing lag is not.
        excess = len(self._pending) - MAX_APPEND_PER_TICK
        if excess > 0:
            for _ in range(excess):
                self._pending.popleft()
            self.dropped += excess

        for stamp, record in self._pending:
            cells = self._cells(stamp, record)
            self._rows.append(cells)
            table.add_row(*cells)
        self._pending.clear()

        self._enforce_cap(table)
        table.scroll_end(animate=False)

    @staticmethod
    def _cells(stamp, record) -> tuple:
        style = "red" if not record.ok else ""
        return (
            stamp.strftime("%H:%M:%S.%f")[:-3],
            Text(record.severity or "-", style=style),
            Text(record.module or "-", style=style),
            Text(record.text, style=style),
        )

    def _enforce_cap(self, table: DataTable) -> None:
        """Shed the oldest rows by rebuilding, never one at a time.

        DataTable.remove_row rebuilds the whole row-index map and bumps the
        update count, which also throws away the ordered-row cache and drags
        the cursor/hover reactives through a refresh. That makes it O(rows):
        measured at ~9ms per call on a 20000-row table. Shedding k rows one at
        a time is therefore O(rows*k), and under a constant stream the drain
        tick ends up spending minutes per 0.1s interval -- the CPU pegs and the
        UI never repaints again.

        clear() + add_rows() is one O(rows) pass instead, so it wins over
        per-row removal for anything past roughly a hundred rows. TRIM_SLACK
        makes sure that is always the case: the table is left to overshoot and
        then sheds the entire overshoot in a single rebuild, so the cost is
        amortized over TRIM_SLACK rows rather than paid per row.
        """
        if table.row_count <= MAX_ROWS + TRIM_SLACK:
            return
        # _rows is capped at MAX_ROWS, so it is already exactly the newest
        # MAX_ROWS rows -- the overshoot is the part it has dropped off its
        # left end.
        table.clear()
        table.add_rows(self._rows)

    def check_catalog(self) -> None:
        """Pick up a rebuild without restarting the monitor."""
        if self.catalog.reload_if_changed():
            self.notify(f"catalog reloaded: {len(self.catalog)} messages")

    # -- actions -----------------------------------------------------------

    def action_toggle_pause(self) -> None:
        self.paused = not self.paused

    def action_clear(self) -> None:
        self.query_one(DataTable).clear()
        self._rows.clear()
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
