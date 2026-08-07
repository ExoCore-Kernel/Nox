#!/usr/bin/env python3
import datetime
import os
import pathlib
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: gen-version.py OUTPUT_C")

root = pathlib.Path(__file__).resolve().parent.parent
build_number = int((root / "twilight" / "build-number.txt").read_text().strip())
now = datetime.datetime.now().astimezone()
build_date = now.strftime("%a %b %e %H:%M:%S %Z %Y")
build_user = os.environ.get("USER") or os.environ.get("LOGNAME") or "root"
build_id = f"tnu-{build_number:05d}.0.0~0"

out = pathlib.Path(sys.argv[1])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(
    '#include <twilight/version.h>\n\n'
    f'const char twilight_build_date[] = "{build_date}";\n'
    f'const char twilight_build_user[] = "{build_user}";\n'
    f'const char twilight_build_id[] = "{build_id}";\n'
)
print(f"Generated Twilight build metadata: {build_id} ({build_date})")
