"""The C files of the given folders under a base folder, one "folder/name" per line, for gn (it has no wildcards)."""
import pathlib
import sys

base = pathlib.Path(sys.argv[1])
for folder in sys.argv[2:]:
    for f in sorted((base / folder).glob("*.c")):
        print(f"{folder}/{f.name}")
