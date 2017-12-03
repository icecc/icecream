#! /bin/bash

prefix="$1"
testdir="$2"
shift
shift
valgrind=
builddir=.
strict=

usage()
{
    echo Usage: "$0 <install_prefix> <testddir> [--builddir=dir] [--valgrind[=command]] [--strict[=value]]"
    exit 3
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
        --strict)
            strict=1
            ;;
        --strict=*)
            strict=`echo $1 | sed 's/^--strict=//'`
            if test "$strict" = "0"; then
                strict=
            fi
            ;;
        *)
            usage
            ;;
    esac
    shift
done

. $builddir/test-setup.sh
if test $? -ne 0; then
    echo Error sourcing test-setup.sh file, aborting.
    exit 4
fi

icecc="${prefix}/bin/icecc"
iceccd="${prefix}/sbin/iceccd"
icecc_scheduler="${prefix}/sbin/icecc-scheduler"
netname="icecctestnetname$$"
protocolversion=$(cat ../services/comm.h | grep '#define PROTOCOL_VERSION ' | sed 's/#define PROTOCOL_VERSION //')

if test -z "$prefix" -o ! -x "$icecc"; then
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
unset ICECC_REMOTE_CPP
unset ICECC_CLANG_REMOTE_CPP
unset ICECC_IGNORE_UNVERIFIED
unset ICECC_EXTRAFILES
unset ICECC_COLOR_DIAGNOSTICS
unset ICECC_CARET_WORKAROUND

mkdir -p "$testdir"

skipped_tests=
chroot_disabled=

check_compilers()
{
    if test -z "$TESTCC"; then
        if cc -v >/dev/null 2>/dev/null; then
            TESTCC=/usr/bin/cc
        elif gcc -v >/dev/null 2>/dev/null; then
            TESTCC=/usr/bin/gcc
        elif clang -v >/dev/null 2>/dev/null; then
            TESTCC=/usr/bin/clang
        else
            echo Cannot find gcc or clang, explicitly set TESTCC.
            exit 5
        fi
    fi
    if test -z "$TESTCXX"; then
        if c++ -v >/dev/null 2>/dev/null; then
            TESTCXX=/usr/bin/c++
        elif g++ -v >/dev/null 2>/dev/null; then
            TESTCXX=/usr/bin/g++
        elif clang -v >/dev/null 2>/dev/null; then
            TESTCXX=/usr/bin/clang++
        else
            echo Cannot find g++ or clang++, explicitly set TESTCXX.
            exit 5
        fi
    fi
    using_gcc=
    if $TESTCC --version | grep gcc >/dev/null; then
        using_gcc=1
    fi
    using_clang=
    if $TESTCC --version | grep clang >/dev/null; then
        using_clang=1
    fi
    echo Using C compiler: $TESTCC
    $TESTCC --version
    if test $? -ne 0; then
        echo Compiler $TESTCC failed.
        exit 5
    fi
    echo Using C++ compiler: $TESTCXX
    $TESTCXX --version
    if test $? -ne 0; then
        echo Compiler $TESTCXX failed.
        exit 5
    fi
    echo
}

abort_tests()
{
    dump_logs
    exit 2
}

start_iceccd()
{
    name=$1
    shift
    ICECC_TEST_SOCKET="$testdir"/socket-${name} ICECC_SCHEDULER=:8767 ICECC_TESTS=1 ICECC_TEST_SCHEDULER_PORTS=8767:8769 \
        $valgrind "${iceccd}" -b "$testdir"/envs-${name} -l "$testdir"/${name}.log -n ${netname} -N ${name}  -v -v -v "$@" 2>>"$testdir"/iceccdstderr_${name}.log &
    pid=$!
    wait_for_proc_sleep 10 ${pid}
    eval ${name}_pid=${pid}
    echo ${pid} > "$testdir"/${name}.pid
}

wait_for_proc_sleep()
{
    local wait_timeout=$1
    shift
    local pid_list="$@"
    local proc_count=$#
    local ps_state_field="state"
    for wait_count in $(seq 1 ${wait_timeout}); do
        local int_sleep_count=$(ps -ho ${ps_state_field} -p ${pid_list} | grep --count "S")
        ((${int_sleep_count} == ${proc_count})) && break
        sleep 1
    done
}

kill_daemon()
{
    daemon=$1

    pid=${daemon}_pid
    if test -n "${!pid}"; then
        kill "${!pid}" 2>/dev/null
        if test $check_type -eq 1; then
            wait ${!pid}
            exitcode=$?
            if test $exitcode -ne 0; then
                echo Daemon $daemon exited with code $exitcode.
                stop_ice 0
                abort_tests
            fi
        fi
    fi
    rm -f "$testdir"/$daemon.pid
    rm -rf "$testdir"/envs-${daemon}
    rm -f "$testdir"/socket-${daemon}
    eval ${pid}=
}

start_ice()
{
    ICECC_TESTS=1 ICECC_TEST_SCHEDULER_PORTS=8767:8769 $valgrind "${icecc_scheduler}" -p 8767 -l "$testdir"/scheduler.log -n ${netname} -v -v -v &
    scheduler_pid=$!
    echo $scheduler_pid > "$testdir"/scheduler.pid

    start_iceccd localice --no-remote -m 2
    start_iceccd remoteice1 -p 10246 -m 2
    start_iceccd remoteice2 -p 10247 -m 2

    notready=
    if test -n "$valgrind"; then
        sleep 10
    else
        sleep 1
    fi
    for time in `seq 1 10`; do
        notready=
        if ! kill -0 $scheduler_pid; then
            echo Scheduler start failure.
            stop_ice 0
            abort_tests
        fi
        for daemon in localice remoteice1 remoteice2; do
            pid=${daemon}_pid
            if ! kill -0 ${!pid}; then
                echo Daemon $daemon start failure.
                stop_ice 0
                abort_tests
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
        abort_tests
    fi
    wait_for_all_daemons_connected
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
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_SCHEDULER=:8767 ICECC_TESTS=1 ICECC_TEST_SCHEDULER_PORTS=8767:8769 \
        $valgrind "${iceccd}" --no-remote -b "$testdir"/envs-localice -l "$testdir"/localice.log -n ${netname} -N localice -m 2 -v -v -v &
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
        abort_tests
    fi
    flush_logs
    if ! grep -q "Netnames:" "$testdir"/localice.log; then
        echo Daemon localice not ready, aborting.
        stop_only_daemon 0
        abort_tests
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
        if test -n "$scheduler_pid"; then
            if ! kill -0 $scheduler_pid; then
                echo Scheduler no longer running.
                stop_ice 0
                abort_tests
            fi
        fi
        for daemon in localice remoteice1 remoteice2; do
            pid=${daemon}_pid
            if ! kill -0 ${!pid}; then
                echo Daemon $daemon no longer running.
                stop_ice 0
                abort_tests
            fi
        done
    fi
    for daemon in localice remoteice1 remoteice2; do
        kill_daemon $daemon
    done
    if test -n "$scheduler_pid"; then
        kill "$scheduler_pid" 2>/dev/null
        if test $check_type -eq 1; then
            wait $scheduler_pid
            exitcode=$?
            if test $exitcode -ne 0; then
                echo Scheduler exited with code $exitcode.
                stop_ice 0
                abort_tests
            fi
        fi
        scheduler_pid=
    fi
    rm -f "$testdir"/scheduler.pid
    stop_secondary_scheduler $check_type
}

stop_secondary_scheduler()
{
    check_type="$1"
    if test $check_type -eq 2; then
        scheduler2_pid=`cat "$testdir"/scheduler2.pid 2>/dev/null`
    fi
    if test $check_type -eq 1; then
        if test -n "$scheduler2_pid"; then
            if ! kill -0 $scheduler2_pid; then
                echo Secondary scheduler no longer running.
                stop_ice 0
                abort_tests
            fi
        fi
    fi
    if test -n "$scheduler2_pid"; then
        kill "$scheduler2_pid" 2>/dev/null
        if test $check_type -eq 1; then
            wait $scheduler2_pid
            exitcode=$?
            if test $exitcode -ne 0; then
                echo Secondary scheduler exited with code $exitcode.
                stop_ice 0
                abort_tests
            fi
        fi
        scheduler2_pid=
    fi
    rm -f "$testdir"/scheduler2.pid
}

stop_only_daemon()
{
    check_first="$1"
    if test $check_first -ne 0; then
        if ! kill -0 $localice_pid; then
            echo Daemon localice no longer running.
            stop_only_daemon 0
            abort_tests
        fi
    fi
    kill $localice_pid 2>/dev/null
    rm -f "$testdir"/localice.pid
    rm -rf "$testdir"/envs-localice
    rm -f "$testdir"/socket-localice
    localice_pid=
}

wait_for_all_daemons_connected()
{
    for secs in `seq 1 10`; do
        flush_logs
        ready=1
        for daemon in localice remoteice1 remoteice2; do
            if ! grep -q "Connected to scheduler" "$testdir"/${daemon}.log; then
                ready=
            fi
        done
        if test -n "$ready"; then
            return
        fi
        sleep 1
    done
    echo Daemons failed to connect to the scheduler.
    stop_ice 0
    abort_tests
}

# First argument is the expected output file, if any (otherwise specify "").
# Second argument is "remote" (should be compiled on a remote host) or "local" (cannot be compiled remotely).
# Third argument is expected exit code - if this is greater than 128 the exit code will be determined by invoking the compiler locally
# Follow optional arguments, in this order:
#   - stderrfix - specifies that the command may result in local recompile because of the gcc stderr workaround.
#   - keepoutput - will keep the file specified using $output (the remotely compiled version)
#   - split_dwarf - compilation is done with -gsplit-dwarf
#   - noresetlogs - will not use reset_logs at the start (needs to be done explicitly before calling run_ice)
#   - remoteabort - remote compilation will abort (as a result of local processing failing and remote daemon killing the remote compiler)
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
        if test -n "$using_gcc"; then
            stderrfix=1
            shift
        fi
    fi
    keepoutput=
    if test "$1" = "keepoutput"; then
        keepoutput=1
        shift
    fi
    split_dwarf=
    if test "$1" = "split_dwarf"; then
        if test -n "$output"; then
            split_dwarf=$(echo $output | sed 's/\.[^.]*//g').dwo
        fi
        shift
    fi
    noresetlogs=
    if test "$1" = "noresetlogs"; then
        noresetlogs=1
        shift
    fi
    remoteabort=
    if test "$1" = "remoteabort"; then
        remoteabort=1
        shift
    fi

    if [[ $expected_exit -gt 128 ]]; then
        $@ 2>/dev/null
        expected_exit=$?
    fi

    if test -z "$noresetlogs"; then
        reset_logs local "$@"
    else
        mark_logs local "$@"
    fi
    echo Running: "$@"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=localice ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "${icecc}" "$@" 2>"$testdir"/stderr.localice

    localice_exit=$?
    if test -n "$output"; then
        mv "$output" "$output".localice
    fi
    if test -n "$split_dwarf"; then
        mv "$split_dwarf" "$split_dwarf".localice
    fi
    cat "$testdir"/stderr.localice >> "$testdir"/stderr.localice.log
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
        mark_logs remote "$@"
        ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "${icecc}" "$@" 2>"$testdir"/stderr.remoteice
        remoteice_exit=$?
        if test -n "$output"; then
            mv "$output" "$output".remoteice
        fi
        if test -n "$split_dwarf"; then
            mv "$split_dwarf" "$split_dwarf".remoteice
        fi
        cat "$testdir"/stderr.remoteice >> "$testdir"/stderr.remoteice.log
        flush_logs
        check_logs_for_generic_errors
        if test "$remote_type" = "remote"; then
            check_log_message icecc "Have to use host 127.0.0.1:10246"
            check_log_error icecc "<building_local>"
            if test -n "$output"; then
                check_log_message remoteice1 "Remote compilation completed with exit code 0"
                check_log_error remoteice1 "Remote compilation aborted with exit code"
                check_log_error remoteice1 "Remote compilation exited with exit code"
            elif test -n "$remoteabort"; then
                check_log_message remoteice1 "Remote compilation aborted with exit code"
                check_log_error remoteice1 "Remote compilation completed with exit code 0"
                check_log_error remoteice1 "Remote compilation exited with exit code"
            else
                check_log_message remoteice1 "Remote compilation exited with exit code $expected_exit"
                check_log_error remoteice1 "Remote compilation completed with exit code 0"
                check_log_error remoteice1 "Remote compilation aborted with exit code"
            fi
        else
            check_log_message icecc "<building_local>"
            check_log_error icecc "Have to use host 127.0.0.1:10246"
        fi
        check_log_error icecc "Have to use host 127.0.0.1:10247"
        check_log_error icecc "building myself, but telling localhost"
        if test -z "$stderrfix"; then
            check_log_error icecc "local build forced"
        else
            check_log_message icecc "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
        fi
    fi

    mark_logs noice "$@"
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
        abort_tests
    fi
    if test $localice_exit -ne $expected_exit; then
        echo "Run without icecc exit code mismatch ($normal_exit vs $expected_exit)"
        stop_ice 0
        abort_tests
    fi
    if test -z "$chroot_disabled" -a "$remoteice_exit" != "$expected_exit"; then
        echo "Remote run exit code mismatch ($remoteice_exit vs $expected_exit)"
        stop_ice 0
        abort_tests
    fi
    if ! diff -q "$testdir"/stderr.localice "$testdir"/stderr; then
        echo "Stderr mismatch ($testdir/stderr.localice)"
        echo ================
        diff -u "$testdir"/stderr "$testdir"/stderr.localice
        echo ================
        stop_ice 0
        abort_tests
    fi
    if test -z "$chroot_disabled"; then
        if ! diff -q "$testdir"/stderr.remoteice "$testdir"/stderr; then
            echo "Stderr mismatch ($testdir/stderr.remoteice)"
            echo ================
            diff -u "$testdir"/stderr "$testdir"/stderr.remoteice
            echo ================
            stop_ice 0
            abort_tests
        fi
    fi

    local remove_offset_number="s/<[A-Fa-f0-9]*>/<>/g"
    local remove_debug_info="s/\(Length\|DW_AT_\(GNU_dwo_\(id\|name\)\|comp_dir\|producer\|linkage_name\|name\)\).*/\1/g"
    local remove_debug_pubnames="/^\s*Offset\s*Name/,/^\s*$/s/\s*[A-Fa-f0-9]*\s*//"
    local remove_size_of_area="s/\(Size of area in.*section:\)\s*[0-9]*/\1/g"
    if test -n "$output"; then
        if grep -q ELF "$output"; then
            readelf -wlLiaprmfFoRt "$output" | sed -e "$remove_debug_info" \
                -e "$remove_offset_number" \
                -e "$remove_debug_pubnames" \
                -e "$remove_size_of_area" > "$output".readelf.txt || cp "$output" "$output".readelf.txt
            readelf -wlLiaprmfFoRt "$output".localice | sed -e "$remove_debug_info" \
                -e "$remove_offset_number" \
                -e "$remove_debug_pubnames" \
                -e "$remove_size_of_area" > "$output".local.readelf.txt || cp "$output" "$output".local.readelf.txt
            if ! diff -q "$output".local.readelf.txt "$output".readelf.txt; then
                echo "Output mismatch ($output.localice)"
                echo ================
                diff -u "$output".readelf.txt "$output".local.readelf.txt
                echo ================
                stop_ice 0
                abort_tests
            fi
            if test -z "$chroot_disabled"; then
                readelf -wlLiaprmfFoRt "$output".remoteice | sed -e "$remove_debug_info" \
                    -e "$remove_offset_number" \
                    -e "$remove_debug_pubnames" \
                    -e "$remove_size_of_area" > "$output".remote.readelf.txt || cp "$output" "$output".remote.readelf.txt
                if ! diff -q "$output".remote.readelf.txt "$output".readelf.txt; then
                    echo "Output mismatch ($output.remoteice)"
                    echo ================
                    diff -u "$output".readelf.txt "$output".remote.readelf.txt
                    echo ================
                    stop_ice 0
                    abort_tests
                fi
            fi
        else
            if ! diff -q "$output".localice "$output"; then
                echo "Output mismatch ($output.localice)"
                echo ================
                diff -u "$output" "$output".localice
                echo ================
                stop_ice 0
                abort_tests
            fi
            if test -z "$chroot_disabled"; then
                if ! diff -q "$output".remoteice "$output"; then
                    echo "Output mismatch ($output.remoteice)"
                    echo ================
                    diff -u "$output" "$output".remoteice
                    echo ================
                    stop_ice 0
                    abort_tests
                fi
            fi
        fi
    fi
    if test -n "$split_dwarf"; then
        readelf -wlLiaprmfFoRt "$split_dwarf" | \
            sed -e "$remove_debug_info" -e "$remove_offset_number" > "$split_dwarf".readelf.txt || cp "$split_dwarf" "$split_dwarf".readelf.txt
        readelf -wlLiaprmfFoRt "$split_dwarf".localice | \
            sed -e $remove_debug_info -e "$remove_offset_number" > "$split_dwarf".local.readelf.txt || cp "$split_dwarf" "$split_dwarf".local.readelf.txt
        if ! diff -q "$split_dwarf".local.readelf.txt "$split_dwarf".readelf.txt; then
            echo "Output DWO mismatch ($split_dwarf.localice)"
            echo ====================
            diff -u "$split_dwarf".readelf.txt "$split_dwarf".local.readelf.txt
            echo ====================
            stop_ice 0
            abort_tests
        fi
        if test -z "$chroot_disabled"; then
            readelf -wlLiaprmfFoRt "$split_dwarf".remoteice | \
                sed -e "$remove_debug_info" -e "$remove_offset_number" > "$split_dwarf".remote.readelf.txt || cp "$split_dwarf" "$split_dwarf".remote.readelf.txt
            if ! diff -q "$split_dwarf".remote.readelf.txt "$split_dwarf".readelf.txt; then
                echo "Output DWO mismatch ($split_dwarf.remoteice)"
                echo ====================
                diff -u "$split_dwarf".readelf.txt "$split_dwarf".remote.readelf.txt
                echo ====================
                stop_ice 0
                abort_tests
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
        rm -f "$output" "$output".localice "$output".readelf.txt "$output".local.readelf.txt "$output".remote.readelf.txt
        if test -n "$keepoutput"; then
            mv "$output".remoteice "$output"
        else
            rm "$output".remoteice
        fi
    fi
    if test -n "$split_dwarf"; then
        rm -f "$split_dwarf" "$split_dwarf".localice "$split_dwarf".remoteice "$split_dwarf".readelf.txt "$split_dwarf".local.readelf.txt "$split_dwarf".remote.readelf.txt
    fi
    rm -f "$testdir"/stderr "$testdir"/stderr.localice "$testdir"/stderr.remoteice
}

make_test()
{
    # make test - actually try something somewhat realistic. Since each node is set up to serve
    # only 2 jobs max, at least some of the 10 jobs should be built remotely.

    # The test is run twice to select different code paths in the scheduler (first time there are no
    # job statistics about nodes, but they will be created for the second run).
    run_number=$1

    echo Running make test $run_number.
    reset_logs "" "make test $run_number"
    wrappers_path=$pkglibexecdir/bin
    if ! test -x "$wrappers_path"/g++; then
            echo "Cannot find $prefix/lib/icecc/bin/g++ , incorrect installation."
            stop_ice 0
            abort_tests
    fi
    make -f Makefile.test OUTDIR="$testdir" clean -s
    PATH="$wrappers_path":/usr/local/bin:/usr/bin:/bin ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log make -f Makefile.test OUTDIR="$testdir" -j10 -s 2>>"$testdir"/stderr.log
    if test $? -ne 0 -o ! -x "$testdir"/maketest; then
        echo Make test $run_number failed.
        stop_ice 0
        abort_tests
    fi
    flush_logs
    check_logs_for_generic_errors
    check_log_message icecc "Have to use host 127.0.0.1:10246"
    check_log_message icecc "Have to use host 127.0.0.1:10247"
    check_log_message_count icecc 1 "<building_local>"
    check_log_message remoteice1 "Remote compilation completed with exit code 0"
    check_log_error remoteice1 "Remote compilation aborted with exit code"
    check_log_error remoteice1 "Remote compilation exited with exit code $expected_exit"
    check_log_message remoteice2 "Remote compilation completed with exit code 0"
    check_log_error remoteice2 "Remote compilation aborted with exit code"
    check_log_error remoteice2 "Remote compilation exited with exit code $expected_exit"
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
    reset_logs "" "icerun${noscheduler} test"
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
            abort_tests
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
            abort_tests
        fi
    done
    if test -z "$seen2"; then
        # Daemon is set up to run 2 jobs max, which means icerun should serialize only up to (and including) 2 jobs at the same time.
        echo "Icerun${noscheduler} test failed, 2 processes were never run at the same time."
        stop_ice 0
        abort_tests
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
            abort_tests
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

# Check that icecc --build-native works.
buildnativetest()
{
    echo Running icecc --build-native test.
    pushd "$testdir" >/dev/null
    compilertype=
    if test -n "$using_clang"; then
        compilertype=clang
    elif test -n "$using_gcc"; then
        compilertype=gcc
    fi
    local tgz=$(PATH="$prefix"/bin:/bin:/usr/bin icecc --build-native $compilertype 2>&1 | \
        grep "^creating .*\.tar\.gz$" | sed -e "s/^creating //")
    if test $? -ne 0; then
        echo icecc --build-native test failed.
        abort_tests
    fi
    rm -f $tgz
    popd >/dev/null
    echo icecc --build-native test successful.
    echo
}

# Check that icecc recursively invoking itself is detected.
recursive_test()
{
    echo Running recursive check test.
    reset_logs "" "recursive check"

    PATH="$prefix"/lib/icecc/bin:"$prefix"/bin:/usr/local/bin:/usr/bin:/bin ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log "${icecc}" ./recursive_g++ -Wall -c plain.c -o plain.o 2>>"$testdir"/stderr.log
    if test $? -ne 111; then
        echo Recursive check test failed.
        stop_ice 0
        abort_tests
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
    reset_logs "" "clang plugin"

    if test -z "$LLVM_CONFIG"; then
        LLVM_CONFIG=llvm-config
    fi
    clangcxxflags=`$LLVM_CONFIG --cxxflags 2>"$testdir"/stderr.log`
    if test $? -ne 0; then
        echo Cannot find Clang development headers, clang plugin test skipped.
        skipped_tests="$skipped_tests clangplugin"
        return
    fi
    echo Clang plugin compile flags: $clangcxxflags
    $TESTCXX -shared -fPIC -g -o "$testdir"/clangplugin.so clangplugin.cpp $clangcxxflags 2>>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo Failed to compile clang plugin, clang plugin test skipped.
        skipped_tests="$skipped_tests clangplugin"
    fi

    # TODO This should be able to also handle the clangpluginextra.txt argument without the absolute path.
    export ICECC_EXTRAFILES=clangpluginextra.txt
    run_ice "$testdir/clangplugintest.o" "remote" 0 $TESTCXX -Wall -c -Xclang -load -Xclang "$testdir"/clangplugin.so \
        -Xclang -add-plugin -Xclang icecreamtest -Xclang -plugin-arg-icecreamtest -Xclang `realpath -s clangpluginextra.txt` \
        clangplugintest.cpp -o "$testdir"/clangplugintest.o
    unset ICECC_EXTRAFILES
    for type in "" ".localice" ".remoteice"; do
        check_section_log_message_count stderr${type} 1 "clangplugintest.cpp:3:5: warning: Icecream plugin found return false"
        check_section_log_message_count stderr${type} 1 "warning: Extra file check successful"
        check_section_log_error stderr${type} "Extra file open error"
        check_section_log_error stderr${type} "Incorrect number of arguments"
        check_section_log_error stderr${type} "File contents do not match"
    done
    echo Clang plugin test successful.
    echo
}

# Both clang and gcc4.8+ produce different debuginfo depending on whether the source file is
# given on the command line or using stdin (which is how icecream does it), so do not compare output.
# But check the functionality is identical to local build.
# 1st argument is compile command, without -o argument.
# 2nd argument is first line of debug at which to start comparing.
debug_test()
{
    compiler="$1"
    args="$2"
    cmd="$1 $2"
    debugstart="$3"
    echo "Running debug test ($cmd)."
    reset_logs "" "debug test ($cmd)"

    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "${icecc}" \
        $cmd -o "$testdir"/debug-remote.o 2>>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo Debug test compile failed.
        stop_ice 0
        abort_tests
    fi

    flush_logs
    check_logs_for_generic_errors
    check_log_message icecc "Have to use host 127.0.0.1:10246"
    check_log_error icecc "Have to use host 127.0.0.1:10247"
    check_log_error icecc "building myself, but telling localhost"
    check_log_error icecc "local build forced"
    check_log_error icecc "<building_local>"
    $compiler -o "$testdir"/debug-remote "$testdir"/debug-remote.o
    if test $? -ne 0; then
        echo Linking in debug test failed.
        stop_ice 0
        abort_tests
    fi
    gdb -nx -batch -x debug-gdb.txt "$testdir"/debug-remote >"$testdir"/debug-stdout-remote.txt  2>/dev/null
    if ! grep -A 1000 "$debugstart" "$testdir"/debug-stdout-remote.txt >"$testdir"/debug-output-remote.txt ; then
        echo "Debug check failed (remote)."
        stop_ice 0
        abort_tests
    fi

    # gcc-4.8+ has -grecord-gcc-switches, which makes the .o differ because of the extra flags the daemon adds,
    # this changes DW_AT_producer and also offsets
    local remove_debug_info="s/\(Length\|DW_AT_\(GNU_dwo_\(id\|name\)\|comp_dir\|producer\|linkage_name\|name\)\).*/\1/g"
    local remove_offset_number="s/<[A-Fa-f0-9]*>/<>/g"
    local remove_size_of_area="s/\(Size of area in.*section:\)\s*[0-9]*/\1/g"
    local remove_debug_pubnames="/^\s*Offset\s*Name/,/^\s*$/s/\s*[A-Fa-f0-9]*\s*//"
    readelf -wlLiaprmfFoRt "$testdir"/debug-remote.o | sed -e 's/offset: 0x[0-9a-fA-F]*//g' \
        -e 's/[ ]*--param ggc-min-expand.*heapsize\=[0-9]\+//g' \
        -e "$remove_debug_info" \
        -e "$remove_offset_number" \
        -e "$remove_size_of_area" \
        -e "$remove_debug_pubnames" > "$testdir"/readelf-remote.txt

    $cmd -o "$testdir"/debug-local.o 2>>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo Debug test compile failed.
        stop_ice 0
        abort_tests
    fi
    $compiler -o "$testdir"/debug-local "$testdir"/debug-local.o
    if test $? -ne 0; then
        echo Linking in debug test failed.
        stop_ice 0
        abort_tests
    fi
    gdb -nx -batch -x debug-gdb.txt "$testdir"/debug-local >"$testdir"/debug-stdout-local.txt 2>/dev/null
    if ! grep -A 1000 "$debugstart" "$testdir"/debug-stdout-local.txt >"$testdir"/debug-output-local.txt ; then
        echo "Debug check failed (local)."
        stop_ice 0
        abort_tests
    fi
    readelf -wlLiaprmfFoRt "$testdir"/debug-local.o | sed -e 's/offset: 0x[0-9a-fA-F]*//g' \
        -e "$remove_debug_info" \
        -e "$remove_offset_number" \
        -e "$remove_size_of_area" \
        -e "$remove_debug_pubnames" > "$testdir"/readelf-local.txt

    if ! diff -q "$testdir"/debug-output-local.txt "$testdir"/debug-output-remote.txt ; then
        echo Gdb output different.
        echo =====================
        diff -u "$testdir"/debug-output-local.txt "$testdir"/debug-output-remote.txt
        echo =====================
        stop_ice 0
        abort_tests
    fi
    if ! diff -q "$testdir"/readelf-local.txt "$testdir"/readelf-remote.txt ; then
        echo Readelf output different.
        echo =====================
        diff -u "$testdir"/readelf-local.txt "$testdir"/readelf-remote.txt
        echo =====================
        stop_ice 0
        abort_tests
    fi
    rm "$testdir"/debug-remote.o "$testdir"/debug-local.o "$testdir"/debug-remote "$testdir"/debug-local "$testdir"/debug-*-*.txt "$testdir"/readelf-*.txt

    echo Debug test successful.
    echo
}

zero_local_jobs_test()
{
    echo Running zero_local_jobs test.

    reset_logs "" "Running zero_local_jobs test"

    kill_daemon localice

    start_iceccd localice --no-remote -m 0

    libdir="${testdir}/libs"
    rm -rf  "${libdir}"
    mkdir "${libdir}"

    mark_logs remote $TESTCXX -Wall -Werror -c testfunc.cpp -o "${testdir}/testfunc.o"
    echo Running: $TESTCXX -Wall -Werror -c testfunc.cpp -o "${testdir}/testfunc.o"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice1 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "${icecc}" $TESTCXX -Wall -Werror -c testfunc.cpp -o "${testdir}/testfunc.o"
    if [[ $? -ne 0 ]]; then
        echo "failed to build testfunc.o"
        grep -q "AddressSanitizer failed to allocate"  "$testdir"/iceccdstderr_remoteice1.log
        if [[ $? ]]; then
            echo "address sanitizer broke, skipping test"
            skipped_tests="$skipped_tests zero_local_jobs_test"
            mark_logs local "skipping zero_local_jobs_test"
            start_iceccd localice --no-remote -m 2
            return 0
        fi
        stop_ice 0
        abort_tests
    fi

    mark_logs remote $TESTCXX -Wall -Werror -c testmainfunc.cpp -o "${testdir}/testmainfunc.o"
    echo Running: $TESTCXX -Wall -Werror -c testmainfunc.cpp -o "${testdir}/testmainfunc.o"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=remoteice2 ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "${icecc}" $TESTCXX -Wall -Werror -c testmainfunc.cpp -o "${testdir}/testmainfunc.o"
    if test $? -ne 0; then
        echo "Error, failed to compile testfunc.cpp"
        stop_ice 0
        abort_tests
    fi

    ar rcs "${libdir}/libtestlib1.a" "${testdir}/testmainfunc.o"
    if test $? -ne 0; then
        echo "Error, 'ar' failed to create the ${libdir}/libtestlib1.a static library from object ${testdir}/testmainfunc.o"
        stop_ice 0
        abort_tests
    fi
    ar rcs "${libdir}/libtestlib2.a" "${testdir}/testfunc.o"
    if test $? -ne 0; then
        echo "Error, 'ar' failed to create the ${libdir}/libtestlib2.a static library from object ${testdir}/testfunc.o"
        stop_ice 0
        abort_tests
    fi

    mark_logs local $TESTCXX -Wall -Werror "-L${libdir}" "-ltestlib1" "-ltestlib2" -o "${testdir}/linkedapp"
    echo Running: $TESTCXX -Wall -Werror "-L${libdir}" "-ltestlib1" "-ltestlib2" -o "${testdir}/linkedapp"
    ICECC_TEST_SOCKET="$testdir"/socket-localice ICECC_TEST_REMOTEBUILD=1 ICECC_PREFERRED_HOST=localice ICECC_DEBUG=debug ICECC_LOGFILE="$testdir"/icecc.log $valgrind "${icecc}" $TESTCXX -Wall -Werror "-L${libdir}" "-ltestlib1" "-ltestlib2" -o "${testdir}/linkedapp" 2>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo "Error, failed to link testlib1 and testlib2 into linkedapp"
        stop_ice 0
        abort_tests
    fi

    "${testdir}/linkedapp" 2>>"$testdir"/stderr.log
    app_ret=$?
    if test ${app_ret} -ne 123; then
        echo "Error, failed to create a test app by building remotely and linking locally"
        stop_ice 0
        abort_tests
    fi
    rm -rf  "${libdir}"

    kill_daemon localice
    start_iceccd localice --no-remote -m 2

    echo zero_local_jobs test successful.
    echo
}

# All log files that are used by tests. Done here to keep the list in just one place.
alltestlogs="scheduler scheduler2 localice remoteice1 remoteice2 icecc stderr stderr.localice stderr.remoteice iceccdstderr_localice iceccdstderr_remoteice1 iceccdstderr_remoteice2"

# Call this at the start of a complete test (e.g. testing a feature). If a test fails, logs before this point will not be dumped.
reset_logs()
{
    type="$1"
    shift
    flush_logs
    for log in $alltestlogs; do
        # save (append) previous log
        cat "$testdir"/${log}_section.log >> "$testdir"/${log}_all.log
        cat "$testdir"/${log}.log >> "$testdir"/${log}_all.log
        rm "$testdir"/${log}_section.log
        rm "$testdir"/${log}.log
        echo -n >"$testdir"/${log}.log
        echo -n >"$testdir"/${log}_section.log
    done
    mark_logs $type "$@"
}

# Call this at the start of a test section (e.g. remote vs local build). Functions such as check_log_message will not check before the mark.
mark_logs()
{
    type="$1"
    shift
    flush_logs
    for log in $alltestlogs; do
        cat "$testdir"/${log}.log >> "$testdir"/${log}_section.log
        # and start a new one
        echo ================ > "$testdir"/${log}.log
        if test -n "$type"; then
            echo "= Test ($type): $@" >> "$testdir"/${log}.log
        else
            echo "= Test : $@" >> "$testdir"/${log}.log
        fi
        echo ================ >> "$testdir"/${log}.log
    done
    flush_logs
}

finish_logs()
{
    for log in $alltestlogs; do
        cat "$testdir"/${log}_section.log >> "$testdir"/${log}_all.log
        cat "$testdir"/${log}.log >> "$testdir"/${log}_section.log
        rm -f "$testdir"/${log}_section.log
        rm -f "$testdir"/${log}.log
    done
    alllogscount=`ls -1 "$testdir"/*_all.log | wc -l`
    logscount=`ls -1 "$testdir"/*.log | wc -l`
    if test $alllogscount -ne $logscount; then
        echo INTERNAL ERROR, unhandled log files:
        ls -1 "$testdir"/*.log | grep -v _all.log
        exit 100
    fi
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

dump_logs()
{
    for log in $alltestlogs; do
        # Skip logs that have only headers
        if grep -q -v "^=" "$testdir"/${log}.log "$testdir"/${log}_section.log; then
            echo ------------------------------------------------
            echo "Log: ${log}"
            cat "$testdir"/${log}_section.log
            cat "$testdir"/${log}.log
        fi
    done
}

check_logs_for_generic_errors()
{
    check_log_error scheduler "that job isn't handled by"
    check_log_error scheduler "the server isn't the same for job"
    check_log_error icecc "got exception "
    check_log_error icecc "found another non option on command line. Two input files"
    check_log_error localice "Ignoring bogus version"
    check_log_error remoteice1 "Ignoring bogus version"
    check_log_error remoteice2 "Ignoring bogus version"
    check_log_error localice "scheduler closed connection"
    check_log_error remoteice1 "scheduler closed connection"
    check_log_error remoteice2 "scheduler closed connection"
    # consider all non-fatal errors such as running out of memory on the remote
    # still as problems, except for:
    # 102 - -fdiagnostics-show-caret forced local build (gcc-4.8+)
    if test -n "$using_gcc"; then
        check_log_error_except icecc "local build forced" "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
    else
        check_log_error icecc "local build forced"
    fi
}

check_log_error()
{
    log="$1"
    if grep -q "$2" "$testdir"/${log}.log; then
        echo "Error, $log log contains error: $2"
        stop_ice 0
        abort_tests
    fi
}

check_section_log_error()
{
    log="$1"
    if grep -q "$2" "$testdir"/${log}.log "$testdir"/${log}_section.log; then
        echo "Error, $log log contains error: $2"
        stop_ice 0
        abort_tests
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
        abort_tests
    fi
}

check_log_message()
{
    log="$1"
    if ! grep -q "$2" "$testdir"/${log}.log; then
        echo "Error, $log log does not contain: $2"
        stop_ice 0
        abort_tests
    fi
}

check_section_log_message()
{
    log="$1"
    if ! grep -q "$2" "$testdir"/${log}.log "$testdir"/${log}_section.log; then
        echo "Error, $log log does not contain: $2"
        stop_ice 0
        abort_tests
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
        abort_tests
    fi
}

check_section_log_message_count()
{
    log="$1"
    expected_count="$2"
    count=`grep "$3" "$testdir"/${log}.log "$testdir"/${log}_section.log | wc -l`
    if test $count -ne $expected_count; then
        echo "Error, $log log does not contain expected count (${count} vs ${expected_count}): $3"
        stop_ice 0
        abort_tests
    fi
}


# ==================================================================
# Main code starts here
# ==================================================================

echo

check_compilers

for log in $alltestlogs; do
    rm -f "$testdir"/${log}.log
    rm -f "$testdir"/${log}_section.log
    rm -f "$testdir"/${log}_all.log
    echo -n >"$testdir"/${log}.log
    echo -n >"$testdir"/${log}_section.log
done

buildnativetest

echo Starting icecream.
stop_ice 2
reset_logs local "Starting"
start_ice
check_logs_for_generic_errors
echo Starting icecream successful.
echo

run_ice "$testdir/plain.o" "remote" 0 $TESTCXX -Wall -Werror -c plain.cpp -o "$testdir/"plain.o

run_ice "$testdir/plain.o" "remote" 0 $TESTCC -Wall -Werror -c plain.c -o "$testdir/"plain.o
run_ice "$testdir/plain.o" "remote" 0 $TESTCXX -Wall -Werror -c plain.cpp -O2 -o "$testdir/"plain.o
run_ice "$testdir/plain.ii" "local" 0 $TESTCXX -Wall -Werror -E plain.cpp -o "$testdir/"plain.ii
run_ice "$testdir/includes.o" "remote" 0 $TESTCXX -Wall -Werror -c includes.cpp -o "$testdir"/includes.o
run_ice "$testdir/plain.o" "local" 0 $TESTCXX -Wall -Werror -c plain.cpp -mtune=native -o "$testdir"/plain.o
run_ice "$testdir/plain.o" "remote" 0 $TESTCC -Wall -Werror -x c++ -c plain -o "$testdir"/plain.o

run_ice "$testdir/testdefine.o" "remote" 0 $TESTCXX -Wall -Werror -DICECREAM_TEST_DEFINE=test -c testdefine.cpp -o "$testdir/"testdefine.o
run_ice "$testdir/testdefine.o" "remote" 0 $TESTCXX -Wall -Werror -D ICECREAM_TEST_DEFINE=test -c testdefine.cpp -o "$testdir/"testdefine.o

run_ice "" "remote" 300 "remoteabort" $TESTCXX -c nonexistent.cpp
run_ice "" "local" 0 /bin/true

run_ice "$testdir/warninginmacro.o" "remote" 0 $TESTCXX -Wall -Wextra -Werror -c warninginmacro.cpp -o "$testdir/"warninginmacro.o

if $TESTCXX -cxx-isystem ./ -fsyntax-only -Werror -c includes.cpp 2>/dev/null; then
    run_ice "$testdir/includes.o" "remote" 0 $TESTCXX -Wall -Werror -cxx-isystem ./ -c includes.cpp -o "$testdir"/includes.o
else
    skipped_tests="$skipped_tests cxx-isystem"
fi

target=`$TESTCXX -v 2>&1 | grep "^Target:" | sed 's/Target://'`
if $TESTCXX -target $target -fsyntax-only -Werror -c plain.cpp 2>/dev/null; then
    run_ice "$testdir/plain.o" "remote" 0 $TESTCXX -Wall -Werror -target $target -c plain.cpp -o "$testdir"/plain.o
else
    skipped_tests="$skipped_tests target"
fi

debug_fission_disabled=
$TESTCXX -E -gsplit-dwarf messages.cpp 2>/dev/null >/dev/null || debug_fission_disabled=1
if test -n "$debug_fission_disabled"; then
    skipped_tests="$skipped_tests split-dwarf"
fi
if test -z "$debug_fission_disabled"; then
    run_ice "$testdir/plain.o" "remote" 0 "split_dwarf" $TESTCXX -Wall -Werror -gsplit-dwarf -g -c plain.cpp -o "$testdir/"plain.o
    run_ice "$testdir/plain.o" "remote" 0 "split_dwarf" $TESTCC -Wall -Werror -gsplit-dwarf -c plain.c -o "$testdir/"plain.o
    run_ice "$testdir/plain.o" "remote" 0 "split_dwarf" $TESTCC -Wall -Werror -gsplit-dwarf -c plain.c -o "../../../../../../../..$testdir/plain.o"
    run_ice "" "remote" 300 "split_dwarf" "remoteabort" $TESTCXX -gsplit-dwarf -c nonexistent.cpp
fi

if test -z "$using_gcc"; then
    run_ice "" "remote" 1 $TESTCXX -c syntaxerror.cpp
    check_section_log_error icecc "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
    run_ice "$testdir/messages.o" "remote" 0 $TESTCXX -Wall -c messages.cpp -o "$testdir"/messages.o
    check_log_message stderr "warning: unused variable 'unused'"
    check_section_log_error icecc "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
else
    if $TESTCXX -E -fdiagnostics-show-caret -Werror messages.cpp >/dev/null 2>/dev/null; then
        # check gcc stderr workaround, icecream will force a local recompile
        run_ice "" "remote" 1 "stderrfix" $TESTCXX -c -fdiagnostics-show-caret syntaxerror.cpp
        run_ice "$testdir/messages.o" "remote" 0 "stderrfix" $TESTCXX -Wall -c -fdiagnostics-show-caret messages.cpp -o "$testdir"/messages.o
        check_log_message stderr "warning: unused variable 'unused'"
        check_section_log_message icecc "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
        # try again without the local recompile
        run_ice "" "remote" 1 $TESTCXX -c -fno-diagnostics-show-caret syntaxerror.cpp
        run_ice "$testdir/messages.o" "remote" 0 $TESTCXX -Wall -c -fno-diagnostics-show-caret messages.cpp -o "$testdir"/messages.o
        check_log_message stderr "warning: unused variable 'unused'"
        check_section_log_error icecc "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
    else
        # This gcc is too old to have this problem, but we do not check the gcc version in icecc.
        run_ice "" "remote" 1 "stderrfix" $TESTCXX -c syntaxerror.cpp
        check_section_log_message icecc "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
        run_ice "$testdir/messages.o" "remote" 0 "stderrfix" $TESTCXX -Wall -c messages.cpp -o "$testdir"/messages.o
        check_log_message stderr "warning: unused variable 'unused'"
        check_section_log_message icecc "local build forced by remote exception: Error 102 - command needs stdout/stderr workaround, recompiling locally"
    fi
fi

if command -v gdb >/dev/null; then
    if command -v readelf >/dev/null; then
        debug_test "$TESTCXX" "-c -g debug.cpp" "Temporary breakpoint 1, main () at debug.cpp:8"
        debug_test "$TESTCXX" "-c -g `pwd`/debug/debug2.cpp" "Temporary breakpoint 1, main () at `pwd`/debug/debug2.cpp:8"
        if test -z "$debug_fission_disabled"; then
            debug_test "$TESTCXX" "-c -g debug.cpp -gsplit-dwarf" "Temporary breakpoint 1, main () at debug.cpp:8"
            debug_test "$TESTCXX" "-c -g `pwd`/debug/debug2.cpp -gsplit-dwarf" "Temporary breakpoint 1, main () at `pwd`/debug/debug2.cpp:8"
        fi
    fi
else
    skipped_tests="$skipped_tests debug"
fi

if $TESTCXX -fsanitize=address -c -fsyntax-only -Werror fsanitize.cpp >/dev/null 2>/dev/null; then
    run_ice "$testdir/fsanitize.o" "remote" 0 keepoutput $TESTCXX -c -fsanitize=address -g fsanitize.cpp -o "$testdir"/fsanitize.o
    $TESTCXX -fsanitize=address -g "$testdir"/fsanitize.o -o "$testdir"/fsanitize 2>>"$testdir"/stderr.log
    if test $? -ne 0; then
        echo "Linking for -fsanitize test failed."
        stop_ice 0
        abort_tests
    fi
    "$testdir"/fsanitize 2>>"$testdir"/stderr.log
    check_log_message stderr "ERROR: AddressSanitizer: heap-use-after-free"
    # Only newer versions of ASAN have the SUMMARY line.
    if grep -q "^SUMMARY:" "$testdir"/stderr.log; then
        check_log_message stderr "SUMMARY: AddressSanitizer: heap-use-after-free .*/fsanitize.cpp:5.* in test_fsanitize_function()"
    fi
    rm "$testdir"/fsanitize.o

    if $TESTCXX -fsanitize=address -fsanitize-blacklist=fsanitize-blacklist.txt -c -fsyntax-only fsanitize.cpp >/dev/null 2>/dev/null; then
        run_ice "" "local" 300 $TESTCXX -c -fsanitize=address -fsanitize-blacklist=nonexistent -g fsanitize.cpp -o "$testdir"/fsanitize.o
        check_section_log_message icecc "file for argument -fsanitize-blacklist=nonexistent missing, building locally"

        run_ice "$testdir/fsanitize.o" "remote" 0 keepoutput $TESTCXX -c -fsanitize=address -fsanitize-blacklist=fsanitize-blacklist.txt -g fsanitize.cpp -o "$testdir"/fsanitize.o
        $TESTCXX -fsanitize=address -fsanitize-blacklist=fsanitize-blacklist.txt  -g "$testdir"/fsanitize.o -o "$testdir"/fsanitize 2>>"$testdir"/stderr.log
        if test $? -ne 0; then
            echo "Linking for -fsanitize test failed."
            stop_ice 0
            abort_tests
        fi
        "$testdir"/fsanitize 2>>"$testdir"/stderr.log
        check_log_error stderr "ERROR: AddressSanitizer: heap-use-after-free"
        if grep -q "^SUMMARY:" "$testdir"/stderr.log; then
            check_log_error stderr "SUMMARY: AddressSanitizer: heap-use-after-free .*/fsanitize.cpp:5 in test()"
        fi
        rm "$testdir"/fsanitize.o
    else
        skipped_tests="$skipped_tests fsanitize-blacklist"
    fi
else
    skipped_tests="$skipped_tests fsanitize"
fi

if test -n "$using_clang"; then
    clangplugintest
else
    skipped_tests="$skipped_tests clangplugin"
fi

icerun_test

recursive_test

if test -z "$chroot_disabled"; then
    # stop the scheduler, to ensure the first make run has no job statistics
    echo Restarting icecream.
    reset_logs local "Restarting"
    stop_ice 1
    start_ice
    check_logs_for_generic_errors
    echo Restarting icecream successful.
    make_test 1
    make_test 2
else
    skipped_tests="$skipped_tests make_test"
fi

if test -z "$chroot_disabled"; then
    zero_local_jobs_test
else
    skipped_tests="$skipped_tests zero_local_jobs_test"
fi

# test -frewrite-includes usage
$TESTCXX -E -Werror -frewrite-includes messages.cpp 2>/dev/null | grep -q '^# 1 "messages.cpp"$' >/dev/null 2>/dev/null
if test $? -eq 0; then
    run_ice "$testdir/messages.o" "remote" 0 $TESTCXX -Wall -c messages.cpp -o "$testdir"/messages.o
    check_log_message stderr "warning: unused variable 'unused'"
else
    echo $TESTCXX does not provide functional -frewrite-includes, skipping test.
    echo
    skipped_tests="$skipped_tests clang_rewrite_includes"
fi

if test -z "$chroot_disabled"; then
    echo Testing different netnames.
    reset_logs remote "Different netnames"
    stop_ice 1
    # Start the secondary scheduler before the primary, so that besides the different netname it would be the preferred scheduler.
    ICECC_TESTS=1 ICECC_TEST_SCHEDULER_PORTS=8767:8769 $valgrind "${icecc_scheduler}" -p 8769 -l "$testdir"/scheduler2.log -n ${netname}_secondary -v -v -v &
    scheduler2_pid=$!
    echo $scheduler2_pid > "$testdir"/scheduler2.pid
    wait_for_proc_sleep 10 ${scheduler2_pid}
    start_ice
    check_log_message scheduler2 "Received scheduler announcement from .* (version $protocolversion, netname ${netname})"
    check_log_error scheduler "has announced itself as a preferred scheduler, disconnecting all connections"
    check_log_message localice "Ignoring scheduler at .*:8769 because of a different netname (${netname}_secondary)"
    check_log_message remoteice1 "Ignoring scheduler at .*:8769 because of a different netname (${netname}_secondary)"
    check_log_message remoteice2 "Ignoring scheduler at .*:8769 because of a different netname (${netname}_secondary)"
    stop_secondary_scheduler 1
    echo Different netnames test successful.
    echo

    echo Testing newer scheduler.
    reset_logs remote "Newer scheduler"
    # Make this scheduler fake its start time, so it should be the preferred scheduler.
    # We could similarly fake the version to be higher, but this should be safer.
    ICECC_TESTS=1 ICECC_TEST_SCHEDULER_PORTS=8767:8769 ICECC_FAKE_STARTTIME=1 \
        $valgrind "${icecc_scheduler}" -p 8769 -l "$testdir"/scheduler2.log -n ${netname} -v -v -v &
    scheduler2_pid=$!
    echo $scheduler2_pid > "$testdir"/scheduler2.pid
    wait_for_proc_sleep 10 ${scheduler2_pid}
    check_log_message scheduler "Received scheduler announcement from .* (version $protocolversion, netname ${netname})"
    check_log_message scheduler "has announced itself as a preferred scheduler, disconnecting all connections"
    check_log_error scheduler2 "has announced itself as a preferred scheduler, disconnecting all connections"
    check_log_message localice "scheduler closed connection"
    check_log_message remoteice1 "scheduler closed connection"
    check_log_message remoteice2 "scheduler closed connection"
    # Daemons will not connect to the secondary debug scheduler (not implemented).
    stop_secondary_scheduler 1
    echo Newer scheduler test successful.
    echo

    echo Testing reconnect.
    reset_logs remote "Reconnect"
    wait_for_all_daemons_connected
    flush_logs
    check_log_message scheduler "login localice protocol version: ${protocolversion}"
    check_log_message scheduler "login remoteice1 protocol version: ${protocolversion}"
    check_log_message scheduler "login remoteice2 protocol version: ${protocolversion}"
    check_log_message localice "Connected to scheduler"
    check_log_message remoteice1 "Connected to scheduler"
    check_log_message remoteice2 "Connected to scheduler"
    echo Reconnect test successful.
    echo
else
    skipped_tests="$skipped_tests scheduler_multiple"
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

ignore=
if test -n "$using_gcc"; then
    # gcc (as of now) doesn't know these options, ignore these tests if they fail
    ignore="cxx-isystem target fsanitize-blacklist clangplugin clang_rewrite_includes"
elif test -n "$using_clang"; then
    # clang seems to handle everything
    ignore=
fi
ignored_tests=
for item in $ignore; do
    if echo " $skipped_tests " | grep -q "$item"; then
        ignored_tests="$ignored_tests $item"
        skipped_tests=`echo $skipped_tests | sed "s/$item//"`
    fi
done

if test -n "$ignored_tests"; then
    echo Ignored tests: $ignored_tests
fi

if test -n "$skipped_tests"; then
    if test -n "$strict"; then
        echo "All executed tests passed, but some were skipped:$skipped_tests"
        echo "Strict mode enabled, failing."
        echo ==================================================
        exit 1
    else
        echo "All tests OK, some were skipped:$skipped_tests"
        echo =================================
    fi
else
    echo All tests OK.
    echo =============
fi

exit 0
