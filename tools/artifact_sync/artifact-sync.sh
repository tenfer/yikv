#!/bin/sh
# Wrapper: same args as artifact_sync.py
exec "$(dirname "$0")/artifact_sync.py" "$@"
