#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${1:-$ROOT/build/windows/OutlastRequeue.exe}"

[[ -f "$BINARY" ]] || { echo "Missing Windows binary: $BINARY" >&2; exit 1; }

FORBIDDEN='SetForegroundWindow|SetActiveWindow|SetFocus|BringWindowToTop|AttachThreadInput|SendInput|keybd_event|mouse_event|GetCursorPos|SetCursorPos|ChangeDisplaySettings|MessageBeep|PlaySound|FlashWindow'
IMPORTS="$(objdump -p "$BINARY")"
if rg -q "$FORBIDDEN" <<< "$IMPORTS"; then
    rg "$FORBIDDEN" <<< "$IMPORTS" >&2
    echo "Forbidden focus, global-input, cursor, or display API imported" >&2
    exit 1
fi

rg -q 'PostMessageW' <<< "$IMPORTS"
rg -q 'Subsystem[[:space:]]+00000002[[:space:]]+\(Windows GUI\)' <<< "$IMPORTS"

SOURCE="$ROOT/src/windows/outlast_requeue_windows.c"
if rg -n "$FORBIDDEN" "$SOURCE"; then
    echo "Forbidden API name present in Windows source" >&2
    exit 1
fi

# Window-management calls may target the controller itself, never the game.
if rg -n 'SetWindowPos\(game_window|ShowWindow\(game_window' "$SOURCE"; then
    echo "Game-targeted window-management call found" >&2
    exit 1
fi

echo "Windows binary contract audit passed: GUI subsystem, targeted PostMessage, no forbidden API imports."
