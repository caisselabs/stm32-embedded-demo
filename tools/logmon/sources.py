#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Where the bytes come from.

A source yields chunks of bytes until it is exhausted (a file) or closed (a
serial port). Keeping this behind one interface is what lets the decoder be
tested against a pty, and lets a captured file be replayed later without the
board.
"""

from __future__ import annotations

import time
from pathlib import Path


class Source:
    name = "source"

    def __iter__(self):
        raise NotImplementedError

    def close(self) -> None:
        pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


class FileSource(Source):
    """Replay a capture (or a `dump binary value` of the ring buffer).

    `realtime` paces the replay so the UI behaves like a live session; the
    default streams as fast as possible.
    """

    def __init__(self, path, chunk: int = 256, delay: float = 0.0):
        self.path = Path(path)
        self.name = str(path)
        self.chunk = chunk
        self.delay = delay

    def __iter__(self):
        with open(self.path, "rb") as f:
            while True:
                data = f.read(self.chunk)
                if not data:
                    return
                yield data
                if self.delay:
                    time.sleep(self.delay)


class SerialSource(Source):
    """A live serial port.

    Note for the NUCLEO-L432KC: /dev/ttyACM0 is the ST-LINK virtual COM port,
    which bridges a real UART (USART2), so the baud here must match the
    firmware's USART2 configuration -- it is not a nominal setting the way it
    is on some USB CDC devices.
    """

    def __init__(self, port: str, baud: int = 115200, timeout: float = 0.1):
        self.port = port
        self.name = f"{port}@{baud}"
        self.baud = baud
        self.timeout = timeout
        self._serial = None

    def open(self):
        import serial  # pyserial; imported late so replay works without it

        self._serial = serial.Serial(
            self.port, self.baud, timeout=self.timeout
        )
        return self._serial

    def __iter__(self):
        s = self._serial or self.open()
        while True:
            # read() returns b"" on timeout, which is how a quiet link looks;
            # yield it so the caller keeps its UI responsive rather than
            # blocking until the target says something.
            data = s.read(4096)
            yield data

    def close(self) -> None:
        if self._serial is not None:
            self._serial.close()
            self._serial = None
