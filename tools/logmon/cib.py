#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Locating and importing CIB's MIPI Sys-T decoder.

The decoding itself is CIB's -- mipi_messages.py knows the packet layouts and
the argument encodings. It ships in the compile-time-init-build checkout, whose
location depends on whether CPM_SOURCE_CACHE was set when the project was
configured, so ask CMake rather than guessing.
"""

from __future__ import annotations

import importlib.util
import os
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

_SUFFIX = Path("python") / "cib"
_CACHE_KEY = "CPM_PACKAGE_compile-time-init-build_SOURCE_DIR:INTERNAL="


def find_build_dir() -> Path:
    override = os.environ.get("BUILD_DIR")
    if override:
        return Path(override)
    for name in ("build", "build-host"):
        candidate = REPO / name
        if (candidate / "CMakeCache.txt").exists():
            return candidate
    return REPO / "build"


def find_cib_python(build: Path | None = None) -> Path | None:
    """Directory holding CIB's mipi_messages.py, or None."""
    override = os.environ.get("CIB_PYTHON")
    if override:
        p = Path(override)
        return p if (p / "mipi_messages.py").exists() else None

    build = build or find_build_dir()
    cache = build / "CMakeCache.txt"
    if cache.exists():
        for line in cache.read_text().splitlines():
            if line.startswith(_CACHE_KEY):
                p = Path(line[len(_CACHE_KEY):].strip()) / _SUFFIX
                if (p / "mipi_messages.py").exists():
                    return p

    p = build / "_deps" / "compile-time-init-build-src" / _SUFFIX
    return p if (p / "mipi_messages.py").exists() else None


class DecoderUnavailable(RuntimeError):
    pass


def load_mipi_messages(build: Path | None = None):
    """Import CIB's mipi_messages module.

    Loaded by path rather than by adding to sys.path, so that a stray
    `mipi_messages.py` elsewhere on the path cannot shadow it.
    """
    directory = find_cib_python(build)
    if directory is None:
        raise DecoderUnavailable(
            "CIB's mipi_messages.py not found. Configure the project "
            "(cmake -B build ...) or set CIB_PYTHON to the directory "
            "containing it."
        )

    name = "cib_mipi_messages"
    if name in sys.modules:
        return sys.modules[name]

    spec = importlib.util.spec_from_file_location(
        name, directory / "mipi_messages.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module
