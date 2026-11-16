#!/bin/sh
set -u

LOG=$1
LIMIT=$2
shift 2

BOOT_DEADLINE=${BOOT_DEADLINE:-30}
IDLE_TIMEOUT=${IDLE_TIMEOUT:-90}
SHUTDOWN_GRACE=${SHUTDOWN_GRACE:-10}

: > "$LOG"
: > "$LOG.qemu"

"$@" >/dev/null 2>"$LOG.qemu" &
pid=$!

start=$(date +%s)
last_change=$start
last_size=0
booted=0
finished=0
finished_at=0

stop_qemu() {
    kill "$pid" 2>/dev/null || true
    i=0
    while [ "$i" -lt 50 ] && kill -0 "$pid" 2>/dev/null; do
        sleep 0.1
        i=$((i + 1))
    done
    kill -9 "$pid" 2>/dev/null || true
}

trap 'stop_qemu; wait "$pid" 2>/dev/null; exit 130' INT TERM

while kill -0 "$pid" 2>/dev/null; do
    now=$(date +%s)

    if [ $((now - start)) -ge "$LIMIT" ]; then
        stop_qemu
        wait "$pid" 2>/dev/null || true
        exit 124
    fi

    if [ -f "$LOG" ]; then
        size=$(wc -c < "$LOG")
        if [ "$size" != "$last_size" ]; then
            last_size=$size
            last_change=$now
        fi

        if [ "$booted" -eq 0 ] && grep -q 'Kyronix Test Runner' "$LOG"; then
            booted=1
            last_change=$now
        fi

        if [ "$booted" -eq 0 ] && [ $((now - start)) -ge "$BOOT_DEADLINE" ]; then
            stop_qemu
            wait "$pid" 2>/dev/null || true
            exit 124
        fi

        if grep -qE 'FATAL:|Kernel panic|PANIC:' "$LOG"; then
            stop_qemu
            wait "$pid" 2>/dev/null || true
            exit 125
        fi

        if [ "$finished" -eq 0 ] && grep -qE 'ALL TESTS PASSED|SOME TESTS FAILED' "$LOG"; then
            finished=1
            finished_at=$now
        fi

        if [ "$finished" -eq 1 ] && [ $((now - finished_at)) -ge "$SHUTDOWN_GRACE" ]; then
            stop_qemu
            wait "$pid" 2>/dev/null || true
            exit 0
        fi

        if [ "$finished" -eq 0 ] && [ $((now - last_change)) -ge "$IDLE_TIMEOUT" ]; then
            stop_qemu
            wait "$pid" 2>/dev/null || true
            exit 124
        fi
    fi

    sleep 0.5
done

wait "$pid"
exit $?
