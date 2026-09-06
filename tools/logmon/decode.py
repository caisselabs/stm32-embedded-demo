#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Turning a raw byte stream from the target into decoded log records.

The firmware currently writes packets to the wire as bare little-endian 32-bit
words with no framing: no delimiter, no length field, no sync marker. That is
the minimal thing the target can do, and it decodes fine as long as the stream
is read from a known packet boundary -- which is the case for a replayed RAM
dump.

A live serial line is not so tidy. We attach mid-stream, the target resets
mid-packet, and a baud mismatch or overrun drops bytes. So this decoder has to
recover on its own:

  * Byte alignment. Attaching mid-word leaves us 1-3 bytes out of phase, and
    every word after that is garbage. detect_alignment() scores all four
    offsets by how many packets each one decodes cleanly.

  * Resync. A packet that fails to decode advances the buffer by one word
    rather than one byte -- with raw words, packets are word-aligned relative
    to one another, so once byte alignment is right the boundaries are too.

  * Incomplete vs corrupt. CIB's decoder raises ValueError("Buffer size too
    small") when it runs off the end of the buffer, which is indistinguishable
    from a genuinely bad packet by exception type alone. They are told apart by
    whether the decoder consumed everything available: if it did, more bytes
    may still arrive and we wait; if bytes remained, the packet is bad. A
    packet that stays undecodable past MAX_PACKET_BYTES is treated as corrupt
    regardless, so an unknown ID at the end of a stream cannot stall forever.

When the firmware grows real framing (COBS was the recommendation), this is the
only module that has to change.
"""

from __future__ import annotations

import itertools
import struct
import time
from dataclasses import dataclass, field

from .cib import load_mipi_messages

WORD_SIZE = 4

# Nothing this project emits comes close; the cap only exists so an
# undecodable tail cannot buffer forever waiting for bytes that would complete
# a packet that was never valid.
MAX_PACKET_BYTES = 256


@dataclass
class Record:
    """One decoded packet, or one resync event."""

    text: str
    raw: bytes
    ok: bool = True
    severity: str | None = None
    module: str | None = None


@dataclass
class Stats:
    bytes_in: int = 0
    packets: int = 0
    errors: int = 0
    resync_bytes: int = 0

    def as_dict(self) -> dict:
        return {
            "bytes": self.bytes_in,
            "packets": self.packets,
            "errors": self.errors,
            "resync": self.resync_bytes,
        }


class _Incomplete(Exception):
    """Not enough bytes yet -- wait for more."""


class _Corrupt(Exception):
    """The bytes present cannot be a valid packet."""


class _Counting:
    """Byte iterator that records how much a decoder consumed.

    CIB's message classes pull bytes off an iterator without reporting how
    many they took, and packet length is not knowable up front -- a Catalog
    message's length depends on the argument list looked up from its ID. This
    is how the buffer learns how far to advance.
    """

    __slots__ = ("_it", "count")

    def __init__(self, it):
        self._it = iter(it)
        self.count = 0

    def __iter__(self):
        return self

    def __next__(self):
        b = next(self._it)
        self.count += 1
        return b


class Decoder:
    """Feed it bytes, get back decoded records."""

    def __init__(self, catalog, mipi=None):
        self.catalog = catalog
        self.mipi = mipi or load_mipi_messages()
        self.types = {
            1: self.mipi.Short32,
            3: self.mipi.Catalog,
            7: self.mipi.Short64,
        }
        self.stats = Stats()
        self._buf = bytearray()

    # -- decoding one packet out of the front of the buffer ----------------

    def _decode_one(self) -> tuple[Record, int]:
        if not self._buf:
            raise _Incomplete

        it = _Counting(self._buf)
        first = next(it)

        cls = self.types.get(first & 0xF)
        if cls is None:
            raise _Corrupt(f"unknown message type {first & 0xF}")

        try:
            msg = cls(
                itertools.chain([first], it),
                self.catalog.messages,
                self.catalog.modules,
                self.catalog.db,
            )
        except (ValueError, AssertionError, StopIteration, KeyError) as e:
            ran_off_the_end = it.count >= len(self._buf)
            if ran_off_the_end and len(self._buf) < MAX_PACKET_BYTES:
                raise _Incomplete from e
            raise _Corrupt(str(e) or type(e).__name__) from e

        n = it.count
        return (
            Record(
                text=str(msg),
                raw=bytes(self._buf[:n]),
                severity=getattr(msg, "severity", None),
                module=getattr(msg, "module", None),
            ),
            n,
        )

    def feed(self, data: bytes) -> list[Record]:
        """Add bytes; return whatever became decodable."""
        self._buf += data
        self.stats.bytes_in += len(data)

        out: list[Record] = []
        while self._buf:
            try:
                record, n = self._decode_one()
            except _Incomplete:
                break
            except _Corrupt as e:
                # Advance one word, not one byte: with raw words the packet
                # boundaries are word-aligned once byte alignment is right.
                skipped = bytes(self._buf[:WORD_SIZE])
                del self._buf[:WORD_SIZE]
                self.stats.errors += 1
                self.stats.resync_bytes += len(skipped)
                out.append(
                    Record(
                        text=f"undecodable: {e} [{skipped.hex(' ')}]",
                        raw=skipped,
                        ok=False,
                    )
                )
                continue

            del self._buf[:n]
            self.stats.packets += 1
            out.append(record)

        return out

    @property
    def pending(self) -> int:
        """Bytes buffered but not yet decodable."""
        return len(self._buf)


def detect_alignment(sample: bytes, catalog, mipi=None, probes: int = 16) -> int:
    """Best byte offset (0-3) at which to start reading `sample`.

    Attaching to a live line mid-word puts every subsequent word out of phase.
    Each offset is scored by how many packets decode cleanly from it; ties go
    to the lower offset, so an already-aligned stream stays at 0.
    """
    best_offset, best_score = 0, -1
    for offset in range(WORD_SIZE):
        decoder = Decoder(catalog, mipi=mipi)
        records = decoder.feed(sample[offset:])
        score = sum(1 for r in records[:probes] if r.ok)
        if score > best_score:
            best_offset, best_score = offset, score
    return best_offset


class Aligner:
    """Holds bytes back until the byte alignment can be scored.

    Attaching to a live line mid-word puts every following word out of phase,
    and the only way to tell is to try decoding from each of the four offsets
    -- which needs traffic. So bytes are buffered until there is enough to
    judge, then released with the winning offset stripped off the front.

    "Enough" cannot be a byte count alone. A target that logs a few packets at
    boot and then goes quiet sits below any threshold forever, and holding its
    bytes back looks exactly like a dead link -- which is the more likely
    reading, so it is the more damaging failure. A quiet gap therefore also
    resolves the alignment, on whatever has arrived by then.

    Scoring on a handful of bytes is less certain than on a full sample. That
    is the right trade: a wrong guess shows up as records flagged bad, which
    is visible and recoverable, whereas showing nothing is neither.
    """

    SAMPLE = 64
    QUIET = 0.5

    def __init__(self, catalog, mipi=None, align=None, clock=time.monotonic):
        self.catalog = catalog
        self.mipi = mipi
        self.align = align
        self._clock = clock
        self._pending = bytearray()
        self._last_data = clock()

    @property
    def resolved(self) -> bool:
        return self.align is not None

    def feed(self, chunk: bytes) -> bytes:
        """Bytes ready to decode, which is b"" while still holding back."""
        if self.align is not None:
            return chunk
        if chunk:
            self._pending += chunk
            self._last_data = self._clock()
        if len(self._pending) >= self.SAMPLE:
            return self._resolve()
        if self._pending and self._clock() - self._last_data >= self.QUIET:
            return self._resolve()
        return b""

    def flush(self) -> bytes:
        """Whatever is still held, at end of stream."""
        if self.align is None and self._pending:
            return self._resolve()
        return b""

    def _resolve(self) -> bytes:
        self.align = detect_alignment(bytes(self._pending), self.catalog,
                                      self.mipi)
        data = bytes(self._pending[self.align:])
        self._pending.clear()
        return data


def words_to_bytes(words) -> bytes:
    """Little-endian word stream, as the target would put it on the wire."""
    return struct.pack(f"<{len(words)}I", *words)
