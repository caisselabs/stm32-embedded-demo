#
# Copyright (c) 2026 Michael Caisse
#
# Distributed under the Boost Software License, Version 1.0.
# (See accompanying file LICENSE_1_0.txt or copy at
# http://www.boost.org/LICENSE_1_0.txt)
#
"""Entry point, so that `python3 tools/logmon` just works.

Running a directory puts that directory on sys.path rather than importing it
as a package, which breaks the relative imports. Fix up the package context
before importing anything from it.
"""

import sys
from pathlib import Path

if __package__ in (None, ""):
    # Run as a directory: import absolutely off the parent instead of
    # rewriting __package__, which leaves __spec__ inconsistent.
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from logmon.cli import main
else:
    from .cli import main

if __name__ == "__main__":
    raise SystemExit(main())
