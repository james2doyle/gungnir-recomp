#!/bin/sh
# Full python suite runner. The suite costs minutes per run, so full output
# always lands in a timestamped log under build/ (gitignored) and failure
# names are reprinted at the end: never truncate the pipe, read the log.
set -u
LOGDIR="${1:-build/test-logs}"
mkdir -p "$LOGDIR"
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LOG="$LOGDIR/python-suite-$STAMP.log"
echo "run_python_suite: logging full output to $LOG"
python3 -m unittest discover -s tests -p "test_*.py" >"$LOG" 2>&1
CODE=$?
grep -E "^(FAIL|ERROR): " "$LOG" | sort -u || true
tail -n 4 "$LOG"
echo "run_python_suite: log $LOG exit=$CODE"
exit $CODE
