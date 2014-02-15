#! /bin/bash

prefix="$1"
testdir="$2"

if test -z "$prefix" -o ! -x "$prefix"/bin/icecc; then
    echo Usage: "$0 <install_prefix> <testdir>"
    exit 1
fi

mkdir -p "$testdir"

start_ice()
{
    "$prefix"/sbin/icecc-scheduler -p 8767 -l "$testdir"/scheduler.log -v -v -v &
    scheduler_pid=$!
    echo $scheduler_pid > "$testdir"/scheduler.pid
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 "$prefix"/sbin/iceccd --no-remote -s localhost:8767 -b "$testdir"/envs-localice -l "$testdir"/localice.log -N localice -v -v -v &
    localice_pid=$!
    echo $localice_pid > "$testdir"/localice.pid
    ICECC_TEST_SOCKET="$testdir"/socket-remoteice1 "$prefix"/sbin/iceccd -p 10246 -s localhost:8767 -b "$testdir"/envs-remoteice1 -l "$testdir"/remoteice1.log -N remoteice1 -v -v -v &
    remoteice1_pid=$!
    echo $remoteice1_pid > "$testdir"/remoteice1.pid
    ICECC_TEST_SOCKET="$testdir"/socket-remoteice2 "$prefix"/sbin/iceccd -p 10247 -s localhost:8767 -b "$testdir"/envs-remoteice2 -l "$testdir"/remoteice2.log -N remoteice2 -v -v -v &
    remoteice2_pid=$!
    echo $remoteice2_pid > "$testdir"/remoteice2.pid
    sleep 5
    if ! kill -0 $scheduler_pid; then
        echo Scheduler start failure.
        stop_ice
        exit 1
    fi
    for daemon in localice remoteice1 remoteice2; do
        pid=${daemon}_pid
        if ! kill -0 ${!pid}; then
            echo Daemon $daemon start failure.
            stop_ice
            exit 1
        fi
    done
}

stop_ice()
{
    check_first="$1"
    scheduler_pid=`cat "$testdir"/scheduler.pid 2>/dev/null`
    localice_pid=`cat "$testdir"/localice.pid 2>/dev/null`
    remoteice1_pid=`cat "$testdir"/remoteice1.pid 2>/dev/null`
    remoteice2_pid=`cat "$testdir"/remoteice2.pid 2>/dev/null`
    if test $check_first -ne 0; then
        if ! kill -0 $scheduler_pid; then
            echo Scheduler no longer running.
            stop_ice 0
            exit 1
        fi
        for daemon in localice remoteice1 remoteice2; do
            pid=${daemon}_pid
            if ! kill -0 ${!pid}; then
                echo Daemon $daemon no longer running.
                stop_ice 0
                exit 1
            fi
        done
    fi
    for daemon in localice remoteice1 remoteice2; do
        pid=${daemon}_pid
        if test -n "${!pid}"; then
            kill "${!pid}" 2>/dev/null
        fi
        rm -f "$testdir"/$daemon.pid
    done
    if test -n "$scheduler_pid"; then
        kill "$scheduler_pid" 2>/dev/null
    fi
    rm -f "$testdir"/scheduler.pid
}

# First argument is the expected output file, if any (otherwise specify "").
# Rest is the command to pass to icecc.
# Command will be run both locally and using icecc and results compared.
run_ice()
{
    output="$1"
    shift
    echo Running: "$@"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_DEBUG=debug "$prefix"/bin/icecc "$@"
    localice_exit=$?
    if test -n "$output"; then
        mv "$output" "$output".localice
    fi
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug "$prefix"/bin/icecc "$@"
    remoteice_exit=$?
    if test -n "$output"; then
        mv "$output" "$output".remoteice
    fi
    "$@"
    normal_exit=$?
    if test $localice_exit -ne $normal_exit; then
        echo "Exit code mismatch ($localice_exit vs $normal_exit)"
        exit 1
    fi
    if test $remoteice_exit -ne $normal_exit; then
        echo "Exit code mismatch ($remoteice_exit vs $normal_exit)"
        exit 1
    fi
    if test -n "$output"; then
        if ! diff -q "$output".localice "$output"; then
            echo "Output mismatch ($output.localice)"
            exit 1
        fi
        if ! diff -q "$output".remoteice "$output"; then
            echo "Output mismatch ($output.remoteice)"
            exit 1
        fi
    fi
    if test $localice_exit -ne 0; then
        echo "Command failed (matches local result, continuing), exit code: $localice_exit"
        echo
    else
        echo Command successful.
        echo
    fi
}

stop_ice 0
start_ice

echo Starting tests.
echo ===============

run_ice "$testdir/plain.o" g++ -Wall -Werror -c plain.cpp -o "$testdir/"plain.o
run_ice "$testdir/plain.ii" g++ -Wall -Werror -E plain.cpp -o "$testdir/"plain.ii
run_ice "" g++ -c nonexistent.cpp

stop_ice 1
echo All tests OK.
echo =============
