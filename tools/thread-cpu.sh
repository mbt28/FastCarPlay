#!/bin/sh
# Per-thread CPU for a running FastCarPlay, sampled from /proc.
#
# Threads are what matter here -- "the app uses 90%" says nothing, but
# "aa-process is at 70% and everything else is idle" points straight at the
# cause. Works on busybox too, where `top -H` does NOT split threads.
#
#   tools/thread-cpu.sh            # sample the running app for 5s
#   tools/thread-cpu.sh 10         # ...for 10s
#   PID=1234 tools/thread-cpu.sh   # a specific process
#
# Run it once in AA USB mode and once with the Carlinkit dongle and compare:
# the thread whose share changes is the one to look at.
set -e

SECONDS_TO_SAMPLE=${1:-5}
PID=${PID:-$(pidof app 2>/dev/null || pidof fastcarplay 2>/dev/null || true)}
PID=${PID%% *}

if [ -z "$PID" ] || [ ! -d "/proc/$PID" ]; then
    echo "error: no running app found (set PID=... to choose one)" >&2
    exit 1
fi

TICKS=$(getconf CLK_TCK 2>/dev/null || echo 100)
echo "pid $PID, sampling ${SECONDS_TO_SAMPLE}s (clock ${TICKS}Hz)"

snapshot() {
    for task in /proc/$PID/task/*; do
        [ -d "$task" ] || continue
        tid=${task##*/}
        # utime + stime are fields 14 and 15, but the comm field can contain
        # spaces, so cut everything up to the closing paren first.
        rest=$(sed 's/.*) //' "$task/stat" 2>/dev/null) || continue
        utime=$(echo "$rest" | cut -d' ' -f12)
        stime=$(echo "$rest" | cut -d' ' -f13)
        name=$(cat "$task/comm" 2>/dev/null || echo "$tid")
        echo "$tid $name $((utime + stime))"
    done
}

snapshot > /tmp/.fcp-cpu-before
sleep "$SECONDS_TO_SAMPLE"
snapshot > /tmp/.fcp-cpu-after

echo
printf '%-8s %-18s %8s\n' TID THREAD "CPU%"
awk -v secs="$SECONDS_TO_SAMPLE" -v ticks="$TICKS" '
    NR == FNR { before[$1] = $3; next }
    {
        delta = $3 - (($1 in before) ? before[$1] : $3)
        pct = delta * 100.0 / (secs * ticks)
        total += pct
        if (pct >= 0.05) printf "%-8s %-18s %7.1f%%\n", $1, $2, pct
    }
    END { printf "%-8s %-18s %7.1f%%\n", "", "TOTAL", total }
' /tmp/.fcp-cpu-before /tmp/.fcp-cpu-after | sort -k3 -rn -t' '

rm -f /tmp/.fcp-cpu-before /tmp/.fcp-cpu-after
