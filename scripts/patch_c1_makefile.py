#!/usr/bin/env python3
"""Byte-exact Makefile patcher: insert src/cli.c into CORE_SRC.

The edit tool normalizes tabs to spaces on every write, which corrupts
make recipes (they must start with a TAB). All Makefile edits go through
python byte-splices instead.
"""
import sys

PATH = "Makefile"

with open(PATH, "rb") as f:
    data = f.read()

OLD = b"CORE_SRC = \\\n\tsrc/arena.c \\\n\tsrc/log.c \\\n"
NEW = b"CORE_SRC = \\\n\tsrc/arena.c \\\n\tsrc/cli.c \\\n\tsrc/log.c \\\n"

if data.count(OLD) != 1:
    sys.exit("anchor not found (or not unique): CORE_SRC head")

data = data.replace(OLD, NEW, 1)

with open(PATH, "wb") as f:
    f.write(data)
print("patched: src/cli.c added to CORE_SRC")
