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
    ICECC_TEST_SOCKET="$testdir"/socket "$prefix"/sbin/iceccd -p 8767 -s localhost -b "$testdir"/envs -l "$testdir"/daemon.log -v -v -v &
    daemon_pid=$!
    echo $daemon_pid > "$testdir"/daemon.pid
    sleep 5
    if ! kill -0 $scheduler_pid; then
        echo Scheduler start failure.
        stop_ice
        exit 1
    fi
    if ! kill -0 $daemon_pid; then
        echo Daemon start failure.
        stop_ice
        exit 1
    fi
}

stop_ice()
{
    check_first="$1"
    daemon_pid=`cat "$testdir"/daemon.pid 2>/dev/null`
    scheduler_pid=`cat "$testdir"/scheduler.pid 2>/dev/null`
    if test $check_first -ne 0; then
        if ! kill -0 $scheduler_pid; then
            echo Scheduler no longer running.
            stop_ice
            exit 1
        fi
        if ! kill -0 $daemon_pid; then
            echo Daemon no longer running.
            stop_ice
            exit 1
        fi
    fi
    if test -n "$daemon_pid"; then
        kill "$daemon_pid" 2>/dev/null
    fi
    rm -f "$testdir"/daemon.pid
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
    ICECC_TEST_SOCKET="$testdir"/socket ICECC_DEBUG=debug "$prefix"/bin/icecc "$@"
    ice_exit=$?
    if test -n "$output"; then
        mv "$output" "$output".ice
    fi
    "$@"
    normal_exit=$?
    if test $ice_exit -ne $normal_exit; then
        echo "Exit code mismatch ($ice_exit vs $normal_exit)"
        exit 1
    fi
    if test -n "$output"; then
        if ! diff -q "$output".ice "$output"; then
            echo "Output mismatch ($output)"
            exit 1
        fi
    fi
    if test $ice_exit -ne 0; then
        echo "Command failed (matches local result, continuing), exit code: $ice_exit"
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
