#!/usr/bin/env python3
"""Make pipe/socket poll yield to the cooperative process scheduler."""
from __future__ import annotations
import pathlib, sys

def rep(t,o,n):
    if o not in t: raise RuntimeError(f"expected cooperative I/O fragment not found: {o[:150]!r}")
    return t.replace(o,n,1)

def main()->int:
    if len(sys.argv)!=2: print(f"usage: {sys.argv[0]} GENERATED_BASH_C",file=sys.stderr); return 2
    p=pathlib.Path(sys.argv[1]); t=p.read_text(encoding="utf-8")
    t=rep(t,"        bool wants_input = false;\n",
          "        bool wants_input = false;\n        bool runtime_wait = false;\n")
    t=rep(t,
        "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
        "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n"
        "                if ((pfd.events & POLLIN) != 0) wants_input = true;\n"
        "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n",
        "            if (plasma_runtime_fd(pfd.fd) != 0) {\n"
        "                (void)plasma_runtime_poll_one(pfd.fd, pfd.events, &pfd.revents);\n"
        "                runtime_wait = true;\n"
        "            } else if (pfd.fd == 0 || pfd.fd == 3) {\n")
    t=rep(t,
        "        if (ready != 0 || timeout == 0 || !wants_input) return ready;\n"
        "        tty_wait_for_input();\n",
        "        if (ready != 0 || timeout == 0 || (!wants_input && !runtime_wait)) return ready;\n"
        "        if (runtime_wait) return ready; /* syscall-boundary scheduler yield */\n"
        "        tty_wait_for_input();\n")
    p.write_text(t,encoding="utf-8"); print(f"Finalized cooperative pipe/socket poll yielding: {p}"); return 0
if __name__=="__main__":
    try: raise SystemExit(main())
    except Exception as e: print(f"ERROR: {e}",file=sys.stderr); raise SystemExit(1)
