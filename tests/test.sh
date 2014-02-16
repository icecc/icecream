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
    echo -n >"$testdir"/scheduler.log
    echo -n >"$testdir"/localice.log
    echo -n >"$testdir"/remoteice1.log
    echo -n >"$testdir"/remoteice2.log
    echo -n >"$testdir"/icecc.log
    "$prefix"/sbin/icecc-scheduler -p 8767 -l "$testdir"/scheduler.log -v -v -v &
    scheduler_pid=$!
    echo $scheduler_pid > "$testdir"/scheduler.pid
    ICECC_TEST_SOCKET="$testdir"/socket-localice "$prefix"/sbin/iceccd --no-remote -s localhost:8767 -b "$testdir"/envs-localice -l "$testdir"/localice.log -N localice -v -v -v &
    localice_pid=$!
    echo $localice_pid > "$testdir"/localice.pid
    ICECC_TEST_SOCKET="$testdir"/socket-remoteice1 "$prefix"/sbin/iceccd -p 10246 -s localhost:8767 -b "$testdir"/envs-remoteice1 -l "$testdir"/remoteice1.log -N remoteice1 -v -v -v &
    remoteice1_pid=$!
    echo $remoteice1_pid > "$testdir"/remoteice1.pid
    ICECC_TEST_SOCKET="$testdir"/socket-remoteice2 "$prefix"/sbin/iceccd -p 10247 -s localhost:8767 -b "$testdir"/envs-remoteice2 -l "$testdir"/remoteice2.log -N remoteice2 -v -v -v &
    remoteice2_pid=$!
    echo $remoteice2_pid > "$testdir"/remoteice2.pid
    notready=
    sleep 1
    for time in `seq 1 5`; do
        notready=
        if ! kill -0 $scheduler_pid; then
            echo Scheduler start failure.
            stop_ice 0
            exit 1
        fi
        for daemon in localice remoteice1 remoteice2; do
            pid=${daemon}_pid
            if ! kill -0 ${!pid}; then
                echo Daemon $daemon start failure.
                stop_ice 0
                exit 1
            fi
            if ! grep -q "Connected to scheduler" "$testdir"/${daemon}.log; then
                # ensure log file flush
                kill -HUP ${!pid}
                grep -q "Connected to scheduler" "$testdir"/${daemon}.log || notready=1
            fi
        done
        if test -z "$notready"; then
            break;
        fi
        sleep 1
    done
    if test -n "$notready"; then
        echo Icecream not ready, aborting.
        stop_ice 0
        exit 1
    fi
    flush_logs
    check_log_error remoteice1 "Cannot use chroot, no remote jobs accepted."
    check_log_error remoteice2 "Cannot use chroot, no remote jobs accepted."
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
        rm -rf "$testdir"/envs-${daemon}
        rm -f "$testdir"/socket-${daemon}
    done
    if test -n "$scheduler_pid"; then
        kill "$scheduler_pid" 2>/dev/null
    fi
    rm -f "$testdir"/scheduler.pid
}

# First argument is the expected output file, if any (otherwise specify "").
# Second argument is "remote" (should be compiled on a remote host) or "local" (cannot be compiled remotely).
# Rest is the command to pass to icecc.
# Command will be run both locally and using icecc and results compared.
run_ice()
{
    output="$1"
    shift
    remote_type="$1"
    shift

    reset_logs local "$@"
    echo Running: "$@"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=localice ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log "$prefix"/bin/icecc "$@"
    localice_exit=$?
    if test -n "$output"; then
        mv "$output" "$output".localice
    fi
    flush_logs
    check_logs_for_generic_errors
    if test "$remote_type" = "remote"; then
        check_log_message icecc "building myself, but telling localhost"
        check_log_error icecc "<building_local>"
    else
        check_log_message icecc "<building_local>"
        check_log_error icecc "building myself, but telling localhost"
    fi
    check_log_error icecc "Have to use host 127.0.0.1:10246"
    check_log_error icecc "Have to use host 127.0.0.1:10247"

    reset_logs remote "$@"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log "$prefix"/bin/icecc "$@"
    remoteice_exit=$?
    if test -n "$output"; then
        mv "$output" "$output".remoteice
    fi
    flush_logs
    check_logs_for_generic_errors
    if test "$remote_type" = "remote"; then
        check_log_message icecc "Have to use host 127.0.0.1:10246"
        check_log_error icecc "<building_local>"
    else
        check_log_message icecc "<building_local>"
        check_log_error icecc "Have to use host 127.0.0.1:10246"
    fi
    check_log_error icecc "Have to use host 127.0.0.1:10247"
    check_log_error icecc "building myself, but telling localhost"

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
    if test -n "$output"; then
        rm "$output" "$output".localice "$output".remoteice
    fi
}

reset_logs()
{
    type="$1"
    shift
    # in case icecc.log doesn't exit, avoid error message
    touch "$testdir"/icecc.log
    for log in scheduler localice remoteice1 remoteice2 icecc; do
        # save (append) previous log
        cat "$testdir"/${log}.log >> "$testdir"/${log}_all.log
        # and start a new one
        echo ============== > "$testdir"/${log}.log
        echo "Test ($type): $@" >> "$testdir"/${log}.log
        echo ============== >> "$testdir"/${log}.log
        if test "$log" != icecc; then
            pid=${log}_pid
            kill -HUP ${!pid}
        fi
    done
}

finish_logs()
{
    for log in scheduler localice remoteice1 remoteice2 icecc; do
        cat "$testdir"/${log}.log >> "$testdir"/${log}_all.log
        rm -f "$testdir"/${log}.log
    done
}

flush_logs()
{
    for daemon in scheduler localice remoteice1 remoteice2; do
        pid=${daemon}_pid
        kill -HUP ${!pid}
    done
}

check_logs_for_generic_errors()
{
    check_log_error scheduler "that job isn't handled by"
    check_log_error scheduler "the server isn't the same for job"
}

check_log_error()
{
    log="$1"
    if grep -q "$2" "$testdir"/${log}.log; then
        echo "Error, $log log contains error: $2"
        exit 1
    fi
}

check_log_message()
{
    log="$1"
    if ! grep -q "$2" "$testdir"/${log}.log; then
        echo "Error, $log log does not contain: $2"
        exit 1
    fi
}

rm -f "$testdir"/scheduler_all.log
rm -f "$testdir"/localice_all.log
rm -f "$testdir"/remoteice1_all.log
rm -f "$testdir"/remoteice2_all.log
rm -f "$testdir"/icecc_all.log

echo Starting icecream.
stop_ice 0
start_ice
check_logs_for_generic_errors

echo Starting tests.
echo ===============
skipped_tests=

run_ice "$testdir/plain.o" "remote" g++ -Wall -Werror -c plain.cpp -o "$testdir/"plain.o
run_ice "$testdir/plain.ii" "local" g++ -Wall -Werror -E plain.cpp -o "$testdir/"plain.ii
run_ice "" "remote" g++ -c nonexistent.cpp
run_ice "" "local" /bin/true

if test -n "`which clang++ 2>/dev/null`"; then
    # There's probably not much point in repeating all tests with Clang, but at least
    # try it works (there's a different icecc-create-env run needed, and -frewrite-includes
    # usage needs checking).
    # Clang writes the input filename in the resulting .o , which means the outputs
    # cannot match (remote node will use stdin for the input, while icecc always
    # builds locally if it itself gets data from stdin). So just do not compare.
    run_ice "" "remote" clang++ -Wall -Werror -c plain.cpp -o "$testdir"/plain.o
    rm "$testdir"/plain.o
else
    skipped_tests="$skipped_tests clang"
fi

reset_logs local "Closing down"
stop_ice 1
check_logs_for_generic_errors
finish_logs

if test -n "$skipped_tests"; then
    echo "All tests OK, some were skipped:$skipped_tests"
    echo =============
else
    echo All tests OK.
    echo =============
fi
