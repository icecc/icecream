#! /bin/bash

prefix="$1"
testdir="$2"
shift
shift
valgrind=
builddir=

usage()
{
    echo Usage: "$0 <install_prefix> <builddir> [--builddir=dir] [--valgrind[=command]]"
    exit 2
}

while test -n "$1"; do
    case "$1" in
        --valgrind)
            valgrind="valgrind --leak-check=no --error-exitcode=10 --suppressions=valgrind_suppressions --log-file=$testdir/valgrind-%p.log --"
            rm -f "$testdir"/valgrind-*.log
            ;;
        --valgrind=*)
            valgrind="`echo $1 | sed 's/^--valgrind=//'` --error-exitcode=10 --suppressions=valgrind_suppressions --log-file=$testdir/valgrind-%p.log --"
            rm -f "$testdir"/valgrind-*.log
            ;;
        --builddir=*)
            builddir=`echo $1 | sed 's/^--builddir=//'`
            ;;
        *)
            usage
            ;;
    esac
    shift
done

if test -z "$prefix" -o ! -x "$prefix"/bin/icecc; then
    usage
fi

# Remote compiler pretty much runs with this setting (and there are no locale files in the chroot anyway),
# so force it also locally, otherwise comparing stderr would easily fail because of locale differences (different quotes).
# Until somebody complains and has a good justification for the effort, don't bother with actually doing
# anything about this for real.
export LC_ALL=C

unset MAKEFLAGS

unset ICECC
unset ICECC_VERSION
unset ICECC_DEBUG
unset ICECC_LOGFILE
unset ICECC_REPEAT_RATE
unset ICECC_PREFERRED_HOST
unset ICECC_CC
unset ICECC_CXX
unset ICECC_CLANG_REMOTE_CPP
unset ICECC_IGNORE_UNVERIFIED
unset ICECC_EXTRAFILES
unset ICECC_COLOR_DIAGNOSTICS
unset ICECC_CARET_WORKAROUND

GCC=/usr/bin/gcc
GXX=/usr/bin/g++
CLANG=/usr/bin/clang
CLANGXX=/usr/bin/clang++

mkdir -p "$testdir"

skipped_tests=
chroot_disabled=

start_ice()
{
    $valgrind "$prefix"/sbin/icecc-scheduler -p 8767 -l "$testdir"/scheduler.log -v -v -v &
    scheduler_pid=$!
    echo $scheduler_pid > "$testdir"/scheduler.pid
    ICECC_TEST_SOCKET="$testdir"/socket-localice $valgrind "$prefix"/sbin/iceccd --no-remote -s localhost:8767 -b "$testdir"/envs-localice -l "$testdir"/localice.log -N localice -m 2 -v -v -v &
    localice_pid=$!
    echo $localice_pid > "$testdir"/localice.pid
    ICECC_TEST_SOCKET="$testdir"/socket-remoteice1 $valgrind "$prefix"/sbin/iceccd -p 10246 -s localhost:8767 -b "$testdir"/envs-remoteice1 -l "$testdir"/remoteice1.log -N remoteice1 -m 2 -v -v -v &
    remoteice1_pid=$!
    echo $remoteice1_pid > "$testdir"/remoteice1.pid
    ICECC_TEST_SOCKET="$testdir"/socket-remoteice2 $valgrind "$prefix"/sbin/iceccd -p 10247 -s localhost:8767 -b "$testdir"/envs-remoteice2 -l "$testdir"/remoteice2.log -N remoteice2 -m 2 -v -v -v &
    remoteice2_pid=$!
    echo $remoteice2_pid > "$testdir"/remoteice2.pid
    notready=
    if test -n "$valgrind"; then
        sleep 10
    else
        sleep 1
    fi
    for time in `seq 1 5`; do
        notready=
        if ! kill -0 $scheduler_pid; then
            echo Scheduler start failure.
            stop_ice 0
            exit 2
        fi
        for daemon in localice remoteice1 remoteice2; do
            pid=${daemon}_pid
            if ! kill -0 ${!pid}; then
                echo Daemon $daemon start failure.
                stop_ice 0
                exit 2
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
        exit 2
    fi
    flush_logs
    grep -q "Cannot use chroot, no remote jobs accepted." "$testdir"/remoteice1.log && chroot_disabled=1
    grep -q "Cannot use chroot, no remote jobs accepted." "$testdir"/remoteice2.log && chroot_disabled=1
    if test -n "$chroot_disabled"; then
        skipped_tests="$skipped_tests CHROOT"
        echo Chroot not available, remote tests will be skipped.
    fi
}

# start only local daemon, no scheduler
start_only_daemon()
{
    ICECC_TEST_SOCKET="$testdir"/socket-localice $valgrind "$prefix"/sbin/iceccd --no-remote -s localhost:8767 -b "$testdir"/envs-localice -l "$testdir"/localice.log -N localice -m 2 -v -v -v &
    localice_pid=$!
    echo $localice_pid > "$testdir"/localice.pid
    if test -n "$valgrind"; then
        sleep 10
    else
        sleep 1
    fi
    if ! kill -0 $localice_pid; then
        echo Daemon localice start failure.
        stop_only_daemon 0
        exit 2
    fi
    flush_logs
    if ! grep -q "Netnames:" "$testdir"/localice.log; then
        echo Daemon localice not ready, aborting.
        stop_only_daemon 0
        exit 2
    fi
}

stop_ice()
{
    # 0 - do not check
    # 1 - check normally
    # 2 - do not check, do not wait (wait would fail, started by previous shell)
    check_type="$1"
    if test $check_type -eq 2; then
        scheduler_pid=`cat "$testdir"/scheduler.pid 2>/dev/null`
        localice_pid=`cat "$testdir"/localice.pid 2>/dev/null`
        remoteice1_pid=`cat "$testdir"/remoteice1.pid 2>/dev/null`
        remoteice2_pid=`cat "$testdir"/remoteice2.pid 2>/dev/null`
    fi
    if test $check_type -eq 1; then
        if ! kill -0 $scheduler_pid; then
            echo Scheduler no longer running.
            stop_ice 0
            exit 2
        fi
        for daemon in localice remoteice1 remoteice2; do
            pid=${daemon}_pid
            if ! kill -0 ${!pid}; then
                echo Daemon $daemon no longer running.
                stop_ice 0
                exit 2
            fi
        done
    fi
    for daemon in localice remoteice1 remoteice2; do
        pid=${daemon}_pid
        if test -n "${!pid}"; then
            kill "${!pid}" 2>/dev/null
            if test $check_type -eq 1; then
                wait ${!pid}
                exitcode=$?
                if test $exitcode -ne 0; then
                    echo Daemon $daemon exited with code $exitcode.
                    stop_ice 0
                    exit 2
                fi
            fi
        fi
        rm -f "$testdir"/$daemon.pid
        rm -rf "$testdir"/envs-${daemon}
        rm -f "$testdir"/socket-${daemon}
        eval ${pid}=
    done
    if test -n "$scheduler_pid"; then
        kill "$scheduler_pid" 2>/dev/null
        if test $check_type -eq 1; then
            wait $scheduler_pid
            exitcode=$?
            if test $exitcode -ne 0; then
                echo Scheduler exited with code $exitcode.
                stop_ice 0
                exit 2
            fi
        fi
        scheduler_pid=
    fi
    rm -f "$testdir"/scheduler.pid
}

stop_only_daemon()
{
    check_first="$1"
    if test $check_first -ne 0; then
        if ! kill -0 $localice_pid; then
            echo Daemon localice no longer running.
            stop_only_daemon 0
            exit 2
        fi
    fi
    kill $localice_pid 2>/dev/null
    rm -f "$testdir"/localice.pid
    rm -rf "$testdir"/envs-localice
    rm -f "$testdir"/socket-localice
    localice_pid=
}


# First argument is the expected output file, if any (otherwise specify "").
# Second argument is "remote" (should be compiled on a remote host) or "local" (cannot be compiled remotely).
# Third argument is expected exit code.
# Fourth argument is optional, "stderrfix" specifies that the command may result in local recompile because of the gcc
# stderr workaround.
# Rest is the command to pass to icecc.
# Command will be run both locally and using icecc and results compared.
run_ice()
{
    output="$1"
    shift
    remote_type="$1"
    shift
    expected_exit=$1
    shift
    stderrfix=
    if test "$1" = "stderrfix"; then
        stderrfix=1
        shift
    fi

    reset_logs local "$@"
    echo Running: "$@"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=localice ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "$prefix"/bin/icecc "$@" 2>"$testdir"/stderr.localice
    localice_exit=$?
    if test -n "$output"; then
        mv "$output" "$output".localice
    fi
    cat "$testdir"/stderr.localice >> "$testdir"/stderr.log
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
    if test -z "$stderrfix"; then
        check_log_error icecc "local build forced"
    fi

    if test -z "$chroot_disabled"; then
        reset_logs remote "$@"
        ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "$prefix"/bin/icecc "$@" 2>"$testdir"/stderr.remoteice
        remoteice_exit=$?
        if test -n "$output"; then
            mv "$output" "$output".remoteice
        fi
        cat "$testdir"/stderr.remoteice >> "$testdir"/stderr.log
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
        if test -z "$stderrfix"; then
            check_log_error icecc "local build forced"
        fi
    fi

    reset_logs noice "$@"
    "$@" 2>"$testdir"/stderr
    normal_exit=$?
    cat "$testdir"/stderr >> "$testdir"/stderr.log
    flush_logs
    check_logs_for_generic_errors
    check_log_error icecc "Have to use host 127.0.0.1:10246"
    check_log_error icecc "Have to use host 127.0.0.1:10247"
    check_log_error icecc "<building_local>"
    check_log_error icecc "building myself, but telling localhost"
    check_log_error icecc "local build forced"

    if test $localice_exit -ne $expected_exit; then
        echo "Local run exit code mismatch ($localice_exit vs $expected_exit)"
        stop_ice 0
        exit 2
    fi
    if test $localice_exit -ne $expected_exit; then
        echo "Run without icecc exit code mismatch ($normal_exit vs $expected_exit)"
        stop_ice 0
        exit 2
    fi
    if test -z "$chroot_disabled" -a "$remoteice_exit" != "$expected_exit"; then
        echo "Remote run exit code mismatch ($remoteice_exit vs $expected_exit)"
        stop_ice 0
        exit 2
    fi
    if ! diff -q "$testdir"/stderr.localice "$testdir"/stderr; then
        echo "Stderr mismatch ($"testdir"/stderr.localice)"
        stop_ice 0
        exit 2
    fi
    if test -z "$chroot_disabled"; then
        if ! diff -q "$testdir"/stderr.remoteice "$testdir"/stderr; then
            echo "Stderr mismatch ($"testdir"/stderr.remoteice)"
            stop_ice 0
            exit 2
        fi
    fi
    if test -n "$output"; then
        if ! diff -q "$output".localice "$output"; then
            echo "Output mismatch ($output.localice)"
            stop_ice 0
            exit 2
        fi
        if test -z "$chroot_disabled"; then
            if ! diff -q "$output".remoteice "$output"; then
                echo "Output mismatch ($output.remoteice)"
                stop_ice 0
                exit 2
            fi
        fi
    fi
    if test $localice_exit -ne 0; then
        echo "Command failed as expected."
        echo
    else
        echo Command successful.
        echo
    fi
    if test -n "$output"; then
        rm -f "$output" "$output".localice "$output".remoteice
    fi
    rm -f "$testdir"/stderr "$testdir"/stderr.localice "$testdir"/stderr.remoteice
}

make_test()
{
    # make test - actually try something somewhat realistic. Since each node is set up to serve
    # only 2 jobs max, at least some of the 10 jobs should be built remotely.

    # All the test compiles are small, and should produce small .o files, which will make the scheduler
    # stats for those jobs, so it will not actually have any statistics about nodes (make_test is intentionally
    # run early to ensure this). So run the test once, first time without stats, second time with stats
    # (make.h has large data to ensure the first make_test run will finally produce statistics).
    run_number=$1

    echo Running make test $run_number.
    reset_logs remote "make test $run_number"
    make -f Makefile.test OUTDIR="$testdir" clean -s
    PATH="$prefix"/lib/icecc/bin:/usr/local/bin:/usr/bin:/bin ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log make -f Makefile.test OUTDIR="$testdir" -j10 -s 2>>"$testdir"/stderr.log
    if test $? -ne 0 -o ! -x "$testdir"/maketest; then
        echo Make test $run_number failed.
        stop_ice 0
        exit 2
    fi
    flush_logs
    check_logs_for_generic_errors
    check_log_message icecc "Have to use host 127.0.0.1:10246"
    check_log_message icecc "Have to use host 127.0.0.1:10247"
    check_log_message_count icecc 1 "<building_local>"
    if test $run_number -eq 1; then
        check_log_message scheduler "no job stats - returning randomly selected"
    else
        check_log_error scheduler "no job stats - returning randomly selected"
    fi
    echo Make test $run_number successful.
    echo
    make -f Makefile.test OUTDIR="$testdir" clean -s
}

# 1st argument, if set, means we run without scheduler
icerun_test()
{
    # test that icerun really serializes jobs and only up to 2 (max jobs of the local daemon) are run at any time
    noscheduler=
    test -n "$1" && noscheduler=" (no scheduler)"
    echo "Running icerun${noscheduler} test."
    reset_logs local "icerun${noscheduler} test"
    # remove . from PATH if set
    save_path=$PATH
    export PATH=`echo $PATH | sed 's/:.:/:/' | sed 's/^.://' | sed 's/:.$//'`
    rm -rf "$testdir"/icerun
    mkdir -p "$testdir"/icerun
    for i in `seq 1 10`; do
        path=$PATH
        if test $i -eq 1; then
            # check icerun with absolute path
            testbin=`pwd`/icerun-test.sh
        elif test $i -eq 2; then
            # check with relative path
            testbin=../tests/icerun-test.sh
        elif test $i -eq 3; then
            # test with PATH
            testbin=icerun-test.sh
            path=`pwd`:$PATH
        else
            testbin=./icerun-test.sh
        fi
        PATH=$path ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "$prefix"/bin/icerun $testbin "$testdir"/icerun $i &
    done
    timeout=100
    seen2=
    while true; do
        runcount=`ls -1 "$testdir"/icerun/running* 2>/dev/null | wc -l`
        if test $runcount -gt 2; then
            echo "Icerun${noscheduler} test failed, more than expected 2 processes running."
            stop_ice 0
            exit 2
        fi
        test $runcount -eq 2 && seen2=1
        donecount=`ls -1 "$testdir"/icerun/done* 2>/dev/null | wc -l`
        if test $donecount -eq 10; then
            break
        fi
        sleep 0.1
        timeout=$((timeout-1))
        if test $timeout -eq 0; then
            echo "Icerun${noscheduler} test timed out."
            stop_ice 0
            exit 2
        fi
    done
    if test -z "$seen2"; then
        # Daemon is set up to run 2 jobs max, which means icerun should serialize only up to (and including) 2 jobs at the same time.
        echo "Icerun${noscheduler} test failed, 2 processes were never run at the same time."
        stop_ice 0
        exit 2
    fi

    # check that plain 'icerun-test.sh' doesn't work for the current directory (i.e. ./ must be required just like with normal execution)
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "$prefix"/bin/icerun icerun-test.sh "$testdir"/icerun 0 &
    icerun_test_pid=$!
    timeout=20
    while true; do
        if ! kill -0 $icerun_test_pid 2>/dev/null; then
            break
        fi
        sleep 0.1
        timeout=$((timeout-1))
        if test $timeout -eq 0; then
            echo "Icerun${noscheduler} test timed out."
            stop_ice 0
            exit 2
        fi
    done
    
    flush_logs
    check_logs_for_generic_errors
    check_log_error icecc "Have to use host 127.0.0.1:10246"
    check_log_error icecc "Have to use host 127.0.0.1:10247"
    check_log_error icecc "building myself, but telling localhost"
    check_log_error icecc "local build forced"
    check_log_message_count icecc 11 "<building_local>"
    check_log_message_count icecc 1 "couldn't find any"
    check_log_message_count icecc 1 "could not find icerun-test.sh in PATH."
    echo "Icerun${noscheduler} test successful."
    echo
    rm -r "$testdir"/icerun
    export PATH=$save_path
}

# Check that icecc recursively invoking itself is detected.
recursive_test()
{
    echo Running recursive check test.
    reset_logs local "recursive check"

    PATH="$prefix"/lib/icecc/bin:"$prefix"/bin:/usr/local/bin:/usr/bin:/bin ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log "$prefix"/bin/icecc ./recursive_g++ -Wall -c plain.c -o plain.o 2>>"$testdir"/stderr.log
    if test $? -ne 111; then
        echo Recursive check test failed.
        stop_ice 0
        exit 2
    fi
    flush_logs
    check_logs_for_generic_errors
    check_log_message icecc "icecream seems to have invoked itself recursively!"
    echo Recursive check test successful.
    echo
}

# Check that transfering Clang plugin(s) works. While at it, also test ICECC_EXTRAFILES.
clangplugintest()
{
    echo Running Clang plugin test.
    reset_logs remote "clang plugin"

    # TODO This should be able to also handle the clangpluginextra.txt argument without the absolute path.
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log ICECC_EXTRAFILES=clangpluginextra.txt $valgrind "$prefix"/bin/icecc \
        $CLANGXX -Wall -c -Xclang -load -Xclang "$builddir"/clangplugin.so -Xclang -add-plugin -Xclang icecreamtest -Xclang -plugin-arg-icecreamtest -Xclang `realpath -s clangpluginextra.txt` clangplugintest.cpp -o "$testdir"/clangplugintest.o 2>>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo Clang plugin test failed.
        stop_ice 0
        exit 2
    fi
    flush_logs
    check_logs_for_generic_errors
    check_log_message icecc "Have to use host 127.0.0.1:10246"
    check_log_error icecc "Have to use host 127.0.0.1:10247"
    check_log_error icecc "building myself, but telling localhost"
    check_log_error icecc "local build forced"
    check_log_error icecc "<building_local>"
    check_log_message_count stderr 1 "clangplugintest.cpp:3:5: warning: Icecream plugin found return false"
    check_log_message_count stderr 1 "warning: Extra file check successful"
    check_log_error stderr "Extra file open error"
    check_log_error stderr "Incorrect number of arguments"
    check_log_error stderr "File contents do not match"
    echo Clang plugin test successful.
    echo
    rm "$testdir"/clangplugintest.o
}

# Both clang and gcc4.8+ produce different debuginfo depending on whether the source file is
# given on the command line or using stdin (which is how icecream does it), so do not compare output.
# But check the functionality is identical to local build.
# 1st argument is compile command, without -o argument.
# 2nd argument is first line of debug at which to start comparing.
debug_test()
{
    cmd="$1"
    debugstart="$2"
    echo "Running debug test ($cmd)."
    reset_logs remote "debug test ($cmd)"

    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "$prefix"/bin/icecc \
        $cmd -o "$testdir"/debug-remote.o 2>>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo Debug test compile failed.
        stop_ice 0
        exit 2
    fi
    flush_logs
    check_logs_for_generic_errors
    check_log_message icecc "Have to use host 127.0.0.1:10246"
    check_log_error icecc "Have to use host 127.0.0.1:10247"
    check_log_error icecc "building myself, but telling localhost"
    check_log_error icecc "local build forced"
    check_log_error icecc "<building_local>"
    $GXX -o "$testdir"/debug-remote "$testdir"/debug-remote.o
    if test $? -ne 0; then
        echo Linking in debug test failed.
        stop_ice 0
        exit 2
    fi
    gdb -nx -batch -x debug-gdb.txt "$testdir"/debug-remote >"$testdir"/debug-stdout-remote.txt 2>/dev/null
    if ! grep -A 1000 "$debugstart" "$testdir"/debug-stdout-remote.txt >"$testdir"/debug-output-remote.txt ; then
        echo "Debug check failed (remote)."
        stop_ice 0
        exit 2
    fi
    # gcc-4.8+ has -grecord-gcc-switches, which makes the .o differ because of the extra flags the daemon adds,
    # this changes DW_AT_producer and also offsets
    readelf -wlLiaprmfFoRt "$testdir"/debug-remote.o | sed 's/offset: 0x[0-9a-fA-F]*//g' | sed 's/[ ]*-fpreprocessed.*$/\t/g' > "$testdir"/readelf-remote.txt

    $cmd -o "$testdir"/debug-local.o 2>>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo Debug test compile failed.
        stop_ice 0
        exit 2
    fi
    $GXX -o "$testdir"/debug-local "$testdir"/debug-local.o
    if test $? -ne 0; then
        echo Linking in debug test failed.
        stop_ice 0
        exit 2
    fi
    gdb -nx -batch -x debug-gdb.txt "$testdir"/debug-local >"$testdir"/debug-stdout-local.txt 2>/dev/null
    if ! grep -A 1000 "$debugstart" "$testdir"/debug-stdout-local.txt >"$testdir"/debug-output-local.txt ; then
        echo "Debug check failed (local)."
        stop_ice 0
        exit 2
    fi
    readelf -wlLiaprmfFoRt "$testdir"/debug-local.o | sed 's/offset: 0x[0-9a-fA-F]*//g' > "$testdir"/readelf-local.txt

    if ! diff -q "$testdir"/debug-output-local.txt "$testdir"/debug-output-remote.txt ; then
        echo Gdb output different.
        stop_ice 0
        exit 2
    fi
    if ! diff -q "$testdir"/readelf-local.txt "$testdir"/readelf-remote.txt ; then
        echo Readelf output different.
        stop_ice 0
        exit 2
    fi
    rm "$testdir"/debug-remote.o "$testdir"/debug-local.o "$testdir"/debug-remote "$testdir"/debug-local "$testdir"/debug-*-*.txt "$testdir"/readelf-*.txt

    echo Debug test successful.
    echo
}

reset_logs()
{
    type="$1"
    shift
    # in case icecc.log or stderr.log don't exit, avoid error message
    touch "$testdir"/icecc.log "$testdir"/stderr.log
    for log in scheduler localice remoteice1 remoteice2 icecc stderr; do
        # save (append) previous log
        cat "$testdir"/${log}.log >> "$testdir"/${log}_all.log
        # and start a new one
        echo ============== > "$testdir"/${log}.log
        echo "Test ($type): $@" >> "$testdir"/${log}.log
        echo ============== >> "$testdir"/${log}.log
        if test "$log" != icecc -a "$log" != stderr; then
            pid=${log}_pid
            if test -n "${!pid}"; then
                kill -HUP ${!pid}
            fi
        fi
    done
}

finish_logs()
{
    for log in scheduler localice remoteice1 remoteice2 icecc stderr; do
        cat "$testdir"/${log}.log >> "$testdir"/${log}_all.log
        rm -f "$testdir"/${log}.log
    done
}

flush_logs()
{
    for daemon in scheduler localice remoteice1 remoteice2; do
        pid=${daemon}_pid
        if test -n "${!pid}"; then
            kill -HUP ${!pid}
        fi
    done
}

check_logs_for_generic_errors()
{
    check_log_error scheduler "that job isn't handled by"
    check_log_error scheduler "the server isn't the same for job"
    check_log_error icecc "got exception "
    # consider all non-fatal errors such as running out of memory on the remote
    # still as problems, except for:
    # 102 - -fdiagnostics-show-caret forced local build (gcc-4.8+)
    check_log_error_except icecc "local build forced" "local build forced by error 102"
}

check_log_error()
{
    log="$1"
    if grep -q "$2" "$testdir"/${log}.log; then
        echo "Error, $log log contains error: $2"
        stop_ice 0
        exit 2
    fi
}

# check the error message ($2) is not present in log ($1),
# but the exception ($3) is allowed
check_log_error_except()
{
    log="$1"
    if cat "$testdir"/${log}.log | grep -v "$3" | grep -q "$2" ; then
        echo "Error, $log log contains error: $2"
        stop_ice 0
        exit 2
    fi
}

check_log_message()
{
    log="$1"
    if ! grep -q "$2" "$testdir"/${log}.log; then
        echo "Error, $log log does not contain: $2"
        stop_ice 0
        exit 2
    fi
}

check_log_message_count()
{
    log="$1"
    expected_count="$2"
    count=`grep "$3" "$testdir"/${log}.log | wc -l`
    if test $count -ne $expected_count; then
        echo "Error, $log log does not contain expected count (${count} vs ${expected_count}): $3"
        stop_ice 0
        exit 2
    fi
}

rm -f "$testdir"/scheduler_all.log
rm -f "$testdir"/localice_all.log
rm -f "$testdir"/remoteice1_all.log
rm -f "$testdir"/remoteice2_all.log
rm -f "$testdir"/icecc_all.log
rm -f "$testdir"/stderr_all.log
echo -n >"$testdir"/scheduler.log
echo -n >"$testdir"/localice.log
echo -n >"$testdir"/remoteice1.log
echo -n >"$testdir"/remoteice2.log
echo -n >"$testdir"/icecc.log
echo -n >"$testdir"/stderr.log

echo Starting icecream.
stop_ice 2
reset_logs local "Starting"
start_ice
check_logs_for_generic_errors
echo Starting icecream successful.
echo

run_ice "$testdir/plain.o" "remote" 0 $GXX -Wall -Werror -c plain.cpp -o "$testdir/"plain.o

if test -z "$chroot_disabled"; then
    make_test 1
    make_test 2
fi

run_ice "$testdir/plain.o" "remote" 0 $GCC -Wall -Werror -c plain.c -o "$testdir/"plain.o
run_ice "$testdir/plain.o" "remote" 0 $GXX -Wall -Werror -c plain.cpp -O2 -o "$testdir/"plain.o
run_ice "$testdir/plain.ii" "local" 0 $GXX -Wall -Werror -E plain.cpp -o "$testdir/"plain.ii
run_ice "$testdir/includes.o" "remote" 0 $GXX -Wall -Werror -c includes.cpp -o "$testdir"/includes.o
run_ice "$testdir/plain.o" "local" 0 $GXX -Wall -Werror -c plain.cpp -mtune=native -o "$testdir"/plain.o
run_ice "$testdir/plain.o" "remote" 0 $GCC -Wall -Werror -x c++ -c plain -o "$testdir"/plain.o
run_ice "" "remote" 1 $GXX -c nonexistent.cpp
run_ice "" "local" 0 /bin/true

if $GXX -E -fdiagnostics-show-caret messages.cpp >/dev/null 2>/dev/null; then
    # gcc stderr workaround, icecream will force a local recompile
    run_ice "" "remote" 1 "stderrfix" $GXX -c syntaxerror.cpp
    run_ice "$testdir/messages.o" "remote" 0 "stderrfix" $GXX -Wall -c messages.cpp -o "$testdir"/messages.o
    check_log_message stderr "warning: unused variable 'unused'"
    # try again without the local recompile
    run_ice "" "remote" 1 $GXX -c -fno-diagnostics-show-caret syntaxerror.cpp
    run_ice "$testdir/messages.o" "remote" 0 $GXX -Wall -c -fno-diagnostics-show-caret messages.cpp -o "$testdir"/messages.o
    check_log_message stderr "warning: unused variable 'unused'"
else
    run_ice "" "remote" 1 $GXX -c syntaxerror.cpp
    run_ice "$testdir/messages.o" "remote" 0 $GXX -Wall -c messages.cpp -o "$testdir"/messages.o
    check_log_message stderr "warning: unused variable 'unused'"
fi

if command -v gdb >/dev/null; then
    if command -v readelf >/dev/null; then
        debug_test "$GXX -c -g debug.cpp" "Temporary breakpoint 1, main () at debug.cpp:8"
        debug_test "$GXX -c -g `pwd`/debug/debug2.cpp" "Temporary breakpoint 1, main () at `pwd`/debug/debug2.cpp:8"
    fi
else
    skipped_tests="$skipped_tests debug"
fi

icerun_test

recursive_test

if test -x $CLANGXX; then
    # There's probably not much point in repeating all tests with Clang, but at least
    # try it works (there's a different icecc-create-env run needed, and -frewrite-includes
    # usage needs checking).
    # Clang writes the input filename in the resulting .o , which means the outputs
    # cannot match (remote node will use stdin for the input, while icecc always
    # builds locally if it itself gets data from stdin). It'd be even worse with -g,
    # since the -frewrite-includes transformation apparently makes the debugginfo
    # differ too (although the end results work just as well). So just do not compare.
    # It'd be still nice to check at least somehow that this really works though.
    run_ice "" "remote" 0 $CLANGXX -Wall -Werror -c plain.cpp -o "$testdir"/plain.o
    rm "$testdir"/plain.o
    run_ice "" "remote" 0 $CLANGXX -Wall -Werror -c includes.cpp -o "$testdir"/includes.o
    rm "$testdir"/includes.o

    # test -frewrite-includes usage
    $CLANGXX -E -Werror -frewrite-includes messages.cpp | head -1 | grep -q '^# 1 "messages.cpp"$' >/dev/null 2>/dev/null
    if test $? -eq 0; then
        run_ice "" "remote" 0 $CLANGXX -Wall -c messages.cpp -o "$testdir"/messages.o
        check_log_message stderr "warning: unused variable 'unused'"
        rm "$testdir"/messages.o
    else
        skipped_tests="$skipped_tests clang_rewrite_includes"
    fi

    if command -v gdb >/dev/null; then
        if command -v readelf >/dev/null; then
            debug_test "$CLANGXX -c -g debug.cpp" "Temporary breakpoint 1, main () at debug.cpp:8"
            debug_test "$CLANGXX -c -g `pwd`/debug/debug2.cpp" "Temporary breakpoint 1, main () at `pwd`/debug/debug2.cpp:8"
        fi
    fi

    if test -n "$builddir" -a -f "$builddir"/clangplugin.so; then
        clangplugintest
    else
        skipped_tests="$skipped_tests clangplugin"
    fi
else
    skipped_tests="$skipped_tests clang"
fi

reset_logs local "Closing down"
stop_ice 1
check_logs_for_generic_errors

reset_logs local "Starting only daemon"
start_only_daemon

# even without scheduler, icerun should still serialize, but still run up to local number of jobs in parallel
icerun_test "noscheduler"

reset_logs local "Closing down (only daemon)"
stop_only_daemon 1

finish_logs

if test -n "$valgrind"; then
    rm -f "$testdir"/valgrind-*.log
fi

if test -n "$skipped_tests"; then
    echo "All tests OK, some were skipped:$skipped_tests"
    echo =============
    exit 1
else
    echo All tests OK.
    echo =============
    exit 0
fi
