#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""The string catalog: message IDs back into text.

IDs are assigned per build, so the catalog has to match the firmware that is
talking. Rebuilding the firmware rewrites log_strings.json, so the file is
reloaded when its mtime changes -- otherwise every message would decode to the
wrong string (or fail outright) after a rebuild, with nothing on screen saying
why.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Catalog:
    path: Path
    db: dict = field(default_factory=dict)
    messages: dict = field(default_factory=dict)
    modules: dict = field(default_factory=dict)
    _mtime: float = 0.0

    @classmethod
    def load(cls, path) -> "Catalog":
        c = cls(path=Path(path))
        c.reload()
        return c

    def reload(self) -> None:
        self.db = json.loads(self.path.read_text())
        # log_decode.py indexes exactly this way; match it so the decoding
        # behaves identically to the offline tool.
        self.messages = {m["id"]: m for m in self.db.get("messages", [])}
        self.modules = {m["id"]: m["string"] for m in self.db.get("modules", [])}
        self._mtime = self.path.stat().st_mtime

    def reload_if_changed(self) -> bool:
        """True if the file changed on disk and was re-read."""
        try:
            mtime = self.path.stat().st_mtime
        except OSError:
            return False
        if mtime == self._mtime:
            return False
        self.reload()
        return True

    def __len__(self) -> int:
        return len(self.messages)
