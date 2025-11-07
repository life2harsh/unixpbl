#!/usr/bin/env bash
msg="${1:-Task update}"
if command -v notify-send >/dev/null 2>&1; then
  notify-send "uxhtop" "$msg"
else
  echo "[uxhtop] $msg"
fi

