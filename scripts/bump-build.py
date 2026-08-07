#!/usr/bin/env python3
import pathlib

root = pathlib.Path(__file__).resolve().parent.parent
path = root / "twilight" / "build-number.txt"
current = int(path.read_text().strip())
new = current + 1
path.write_text(f"{new}\n")
print(f"Twilight build number: {current} -> {new}")
