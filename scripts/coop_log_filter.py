#!/usr/bin/env python3
"""Filter a Dusk log down to co-op-multiplayer-relevant lines.

A full Dusk log is mostly `Loading Resource:` / `initTexObj:` / fpc create
churn. When debugging the co-op subsystem (`[coop]` tagged lines, the puppet,
cutscene/event transitions, save sync, crashes) that's noise. This keeps just
the signal so you can paste a tight excerpt.

Usage:
    python scripts/coop_log_filter.py [LOGFILE] [--tail N]

  LOGFILE   path to a `dusk-YYYYMMDD-HHMMSS.log`. If omitted, reads stdin;
            if stdin is a tty, auto-picks the newest log under
            %APPDATA%\\TwilitRealm\\Dusk\\logs.
  --tail N  keep only the last N matching lines.

Example:
    python scripts/coop_log_filter.py "$env:APPDATA\\TwilitRealm\\Dusk\\logs\\dusk-...log"
    Get-Content path\\to\\dusk-...log | python scripts/coop_log_filter.py --tail 300
"""
import argparse
import glob
import os
import re
import sys

# A line is kept if it matches any of these (case-sensitive where it matters).
# NOTE: don't match a bare "osReport" — every [INFO | dusk::osReport] line
# (thread init, JKRHeap, ...) would slip through; the interesting osReport lines
# are errors (caught by \[ERROR…) or contain デモ.
KEEP = re.compile(
    r"\[coop\]"                       # all co-op debug logging
    r"|dusk::net"                     # net/replication/save_sync (pre-[coop]-tag lines)
    r"|ALINK create"                  # daAlink_c::create entry log
    r"|executePuppet"                 # legacy puppet-exec log
    r"|frameInterp:"                  # event/cutscene type transitions
    r"|デモ"                          # demo-data errors (デモデータ読み込みエラー …)
    r"|dDemo_c::|dEvent|evmng"        # demo / event manager activity
    r"|U_GetAtanTable"                # the wolf-anim / bad-idx warning
    r"|\[ERROR|\[WARNING|\[FATAL"     # any error/warning/fatal log line
    r"|fpcNm_ALINK_e|fpcNm_DEMO00_e|fpcNm_MIDNA_e"  # Link / cutscene-Link / Midna actor creates
    r"|fopScnRq|fopScnM_CreateReq|PLAY_SCENE"        # scene/stage transition markers
)


# Dusk's config/log dir is "<APPDATA>\TwilitRealm\Dusk"; logs are under "logs".
LOG_DIR_PARTS = ("TwilitRealm", "Dusk", "logs")


def autopick_log():
    for env in ("APPDATA", "LOCALAPPDATA"):
        base = os.environ.get(env)
        if not base:
            continue
        candidates = glob.glob(os.path.join(base, *LOG_DIR_PARTS, "dusk-*.log"))
        if candidates:
            return max(candidates, key=os.path.getmtime)
    return None


def main():
    # The log can contain non-cp1252 chars (デモデータ…); don't let printing them
    # crash on a Windows console.
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, ValueError):
        pass

    ap = argparse.ArgumentParser(description="Filter a Dusk log to co-op lines.")
    ap.add_argument("logfile", nargs="?", help="path to a dusk-*.log (default: stdin / autodetect)")
    ap.add_argument("--tail", type=int, default=0, help="keep only the last N matching lines")
    args = ap.parse_args()

    if args.logfile:
        src = open(args.logfile, "r", encoding="utf-8", errors="replace")
    elif not sys.stdin.isatty():
        src = sys.stdin
    else:
        picked = autopick_log()
        if not picked:
            ap.error("no logfile given, stdin is a tty, and no dusk-*.log found "
                     "under %APPDATA%\\TwilitRealm\\Dusk\\logs")
        sys.stderr.write(f"# reading {picked}\n")
        src = open(picked, "r", encoding="utf-8", errors="replace")

    total = 0
    kept = []
    for line in src:
        total += 1
        if KEEP.search(line):
            kept.append(line.rstrip("\n"))
    if src is not sys.stdin:
        src.close()

    out = kept[-args.tail:] if args.tail > 0 else kept
    for line in out:
        print(line)
    sys.stderr.write(f"# coop_log_filter: kept {len(out)} of {total} lines\n")


if __name__ == "__main__":
    main()
