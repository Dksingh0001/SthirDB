#!/usr/bin/env bash
# Snapshot ./data into backups/data_<label>.tar.gz (fixed name; re-runs overwrite).
# Usage: scripts/backup_data.sh <label>    e.g. scripts/backup_data.sh phase1
set -euo pipefail

LABEL="${1:?usage: backup_data.sh <label>}"

if [ ! -d data ]; then
  echo "backup: no ./data directory to back up" >&2
  exit 1
fi

mkdir -p backups
OUT="backups/data_${LABEL}.tar.gz"
tar -czf "$OUT" data
echo "backup: $OUT ($(du -h "$OUT" | cut -f1))"
