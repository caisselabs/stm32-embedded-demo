#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Command line for the log monitor."""

from __future__ import annotations

import argparse
import sys

from .catalog import Catalog
from .cib import DecoderUnavailable, find_build_dir, load_mipi_messages
from .decode import Decoder, detect_alignment
from .sources import FileSource, SerialSource

DEFAULT_PORT = "/dev/ttyACM0"
DEFAULT_BAUD = 115200


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="logmon",
        description="Live-decode CIB binary logs from the target.",
    )
    src = p.add_mutually_exclusive_group()
    src.add_argument(
        "--port",
        nargs="?",
        const=DEFAULT_PORT,
        help=f"serial port to read (default {DEFAULT_PORT} when given bare)",
    )
    src.add_argument(
        "--replay",
        help="replay a captured byte stream instead of reading a port",
    )
    p.add_argument(
        "--catalog",
        help="log_strings.json for the running firmware "
             "(default: <build>/log_strings.json)",
    )
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument(
        "--capture",
        help="also write every received byte here, for later --replay",
    )
    p.add_argument(
        "--align",
        default=None,
        help="byte alignment: 0-3, or 'auto' to detect (default: auto for a "
             "port, 0 for a replay, which starts on a packet boundary)",
    )
    p.add_argument(
        "--plain",
        action="store_true",
        help="print lines to stdout instead of starting the TUI",
    )
    p.add_argument(
        "--replay-delay",
        type=float,
        default=0.0,
        help="seconds to pause between replay chunks, to pace the UI",
    )
    return p


def resolve_catalog(args) -> Catalog:
    path = args.catalog or (find_build_dir() / "log_strings.json")
    try:
        return Catalog.load(path)
    except FileNotFoundError:
        raise SystemExit(
            f"catalog not found: {path}\n"
            "Build the project, or pass --catalog with the log_strings.json "
            "that matches the running firmware."
        )


def make_source(args):
    if args.replay:
        return FileSource(args.replay, delay=args.replay_delay)
    port = args.port or DEFAULT_PORT
    return SerialSource(port, args.baud)


def initial_alignment(args) -> int | str:
    if args.align is None:
        # Unspecified: a replay starts at a packet boundary by construction,
        # only a live attach can land mid-word. An explicit --align auto still
        # forces detection, which is useful on a capture taken mid-stream.
        return 0 if args.replay else "auto"
    if args.align != "auto":
        try:
            value = int(args.align)
        except ValueError:
            raise SystemExit("--align must be 0, 1, 2, 3 or 'auto'")
        if not 0 <= value <= 3:
            raise SystemExit("--align must be 0, 1, 2, 3 or 'auto'")
        return value
    return "auto"


def run_plain(args) -> int:
    catalog = resolve_catalog(args)
    try:
        mipi = load_mipi_messages()
    except DecoderUnavailable as e:
        raise SystemExit(str(e))

    decoder = Decoder(catalog, mipi=mipi)
    align = initial_alignment(args)
    source = make_source(args)
    capture = open(args.capture, "wb") if args.capture else None

    print(
        f"# source={source.name} catalog={catalog.path} "
        f"({len(catalog)} messages)",
        file=sys.stderr,
    )

    def emit(records):
        for record in records:
            prefix = "" if record.ok else "!! "
            print(f"{prefix}{record.text}", flush=True)

    # Alignment is scored on real traffic, so hold bytes back until there is
    # enough to judge -- or until the stream ends, whichever comes first. A
    # short capture must still be decoded, not silently swallowed.
    pending = bytearray()
    resolved = align if align != "auto" else None
    SAMPLE = 64

    def resolve_alignment():
        nonlocal resolved
        resolved = detect_alignment(bytes(pending), catalog, mipi)
        print(f"# byte alignment: {resolved}", file=sys.stderr)
        data = bytes(pending[resolved:])
        pending.clear()
        return data

    try:
        with source:
            for chunk in source:
                if capture and chunk:
                    capture.write(chunk)
                    capture.flush()

                if resolved is None:
                    pending += chunk
                    if len(pending) < SAMPLE:
                        continue
                    chunk = resolve_alignment()

                emit(decoder.feed(chunk))

            if resolved is None and pending:
                emit(decoder.feed(resolve_alignment()))
    except KeyboardInterrupt:
        pass
    finally:
        if capture:
            capture.close()

    s = decoder.stats
    print(
        f"# {s.packets} packets, {s.errors} errors, "
        f"{s.resync_bytes} bytes discarded, {decoder.pending} unconsumed",
        file=sys.stderr,
    )
    return 0


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)

    if args.plain:
        return run_plain(args)

    try:
        from .tui import run_tui
    except ImportError:
        print(
            "textual is not installed; falling back to --plain.\n"
            "  sudo apt install python3-textual",
            file=sys.stderr,
        )
        return run_plain(args)
    return run_tui(args)


if __name__ == "__main__":
    raise SystemExit(main())
