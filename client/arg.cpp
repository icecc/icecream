/* -*- mode: C++; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 99; -*- */
/* vim: set ts=4 sw=4 et tw=99:  */
/*
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>

#include "client.h"

using namespace std;

// Whether any option controlling color output has been explicitly given.
bool explicit_color_diagnostics;

// Whether -fno-diagnostics-show-caret was given.
bool explicit_no_show_caret;

#define CLIENT_DEBUG 0

inline bool str_equal(const char* a, const char* b)
{
    return strcmp(a, b) == 0;
}

inline int str_startswith(const char *head, const char *worm)
{
    return !strncmp(head, worm, strlen(head));
}

/* Some files should always be built locally... */
static bool
should_always_build_locally(const string &filepath)
{
    string p = find_basename(filepath);
    const char *filename = p.c_str();

    /* autoconf */
    if (str_startswith("conftest.", filename)
        || str_startswith("tmp.conftest.", filename)) {
        return true;
    }

    static const char* const cmake_checks[] = {
        "CheckIncludeFile.",
        "CheckFunctionExists.",
        "CheckSymbolExists.",
        "CheckTypeSize.",
        "CheckPrototypeDefinition.",
    };

    /* cmake */
    if (str_startswith("Check", filename)) {
        for(const char* const cmake_check : cmake_checks) {
            if (str_startswith( cmake_check, filename)) {
                return true;
            }
        }
    }

    return false;
}

static bool analyze_program(const char *name, CompileJob &job, bool& icerun)
{
    string compiler_name = find_basename(name);

    job.setCompilerName(compiler_name);

    if( icerun ) {
        job.setLanguage(CompileJob::Lang_Custom);
        log_info() << "icerun, running locally." << endl;
        return true;
    } else if( is_cpp_compiler(compiler_name)) {
        job.setLanguage(CompileJob::Lang_CXX);
    } else if( is_c_compiler(compiler_name)) {
        job.setLanguage(CompileJob::Lang_C);
    } else {
        job.setLanguage(CompileJob::Lang_Custom);
        log_info() << "custom command, running locally." << endl;
        icerun = true;
        return true;
    }

    return false;
}

static bool is_argument_with_space(const char* argument)
{
    // List taken from https://clang.llvm.org/docs/genindex.html
    // TODO: Add support for arguments with two or three values
    //         -sectalign <arg1> <arg2> <arg3>
    //         -sectcreate <arg1> <arg2> <arg3>
    //         -sectobjectsymbols <arg1> <arg2>
    //         -sectorder <arg1> <arg2> <arg3>
    //         -segaddr <arg1> <arg2>
    //         -segcreate <arg1> <arg2> <arg3>
    //         -segprot <arg1> <arg2> <arg3>
    //       Move some arguments to Arg_Cpp or Arg_Local
    static const char* const arguments[] = {
        "-dyld-prefix",
        "-gcc-toolchain",
        "--param",
        "--sysroot",
        "--system-header-prefix",
        "-target",
        "--assert",
        "--allowable_client",
        "-arch",
        "-arch_only",
        "-arcmt-migrate-report-output",
        "--prefix",
        "-bundle_loader",
        "-dependency-dot",
        "-dependency-file",
        "-dylib_file",
        "-exported_symbols_list",
        "--bootclasspath",
        "--CLASSPATH",
        "--classpath",
        "--resource",
        "--encoding",
        "--extdirs",
        "-filelist",
        "-fmodule-implementation-of",
        "-fmodule-name",
        "-fmodules-user-build-path",
        "-fnew-alignment",
        "-force_load",
        "--output-class-directory",
        "-framework",
        "-frewrite-map-file",
        "-ftrapv-handler",
        "-image_base",
        "-init",
        "-install_name",
        "-lazy_framework",
        "-lazy_library",
        "-meabi",
        "-mhwdiv",
        "-mllvm",
        "-module-dependency-dir",
        "-mthread-model",
        "-multiply_defined",
        "-multiply_defined_unused",
        "-rpath",
        "--rtlib",
        "-seg_addr_table",
        "-seg_addr_table_filename",
        "-segs_read_only_addr",
        "-segs_read_write_addr",
        "-serialize-diagnostics",
        "--serialize-diagnostics",
        "-std",
        "--stdlib",
        "--force-link",
        "-umbrella",
        "-unexported_symbols_list",
        "-weak_library",
        "-weak_reference_mismatches",
        "-B",
        "-D",
        "-U",
        "-I",
        "-i",
        "--include-directory",
        "-L",
        "-l",
        "--library-directory",
        "-MF",
        "-MT",
        "-MQ",
        "-cxx-isystem",
        "-c-isystem",
        "-idirafter",
        "--include-directory-after",
        "-iframework",
        "-iframeworkwithsysroot",
        "-imacros",
        "-imultilib",
        "-iprefix",
        "--include-prefix",
        "-iquote",
        "-include",
        "-include-pch",
        "-isysroot",
        "-isystem",
        "-isystem-after",
        "-ivfsoverlay",
        "-iwithprefix",
        "--include-with-prefix",
        "--include-with-prefix-after",
        "-iwithprefixbefore",
        "--include-with-prefix-before",
        "-iwithsysroot"
    };

    for(const char* const arg : arguments) {
        if (str_equal( arg, argument)) {
            return true;
        }
    }

    return false;
}

static bool analyze_assembler_arg(string &arg, list<string> *extrafiles)
{
    const char *pos = arg.c_str();
    static bool second_option;

    if (second_option) {
        second_option = false;
        return false;
    }

    if (str_startswith("-a", pos)) {
        /* -a[a-z]*=output, which directs the listing to the named file
         * and cannot be remote.
         */
        pos += 2;

        while ((*pos >= 'a') && (*pos <= 'z')) {
            pos++;
        }

        return *pos == '=';
    } else if (str_equal("--debug-prefix-map", pos) ||
               str_equal("--defsym", pos)) {
        second_option = true;
        return false;
    } else if (pos[0] == '@') {
        /* If a build system passes an @FILE argument we'd need to
         * parse the file for more arguments. Instead, we'll just
         * run locally.
         */
        return true;
    } else if (arg[0] == '-') {
        /* All other option arguments should be safe to run remotely. */
        return false;
    } else {
        /* Some build systems pass directly additional assembler files.
         * Example: -Wa,src/code16gcc.s
         * Thus, if any option doesn't start with a dash we need to
         * add an extra file to the compile step.
         */

        if (access(arg.c_str(), R_OK) == 0) {
            arg = get_absfilename(arg);
            extrafiles->push_back(arg);
            return false;
        } else {
            log_warning() << "file for argument missing, building locally" << endl;
            return true;
        }

        return false;
    }
}


int analyse_argv(const char * const *argv, CompileJob &job, bool icerun, list<string> *extrafiles)
{
    ArgumentsList args;
    string ofile;

#if CLIENT_DEBUG > 1
    trace() << "scanning arguments" << endl;

    for (int index = 0; argv[index]; index++) {
        trace() << " " << argv[index] << endl;
    }

    trace() << endl;
#endif

    bool had_cc = (job.compilerName().size() > 0);
    bool always_local = analyze_program(had_cc ? job.compilerName().c_str() : argv[0], job, icerun);
    bool seen_c = false;
    bool seen_s = false;
    bool seen_mf = false;
    bool seen_md = false;
    bool seen_split_dwarf = false;
    bool seen_target = false;
    bool wunused_macros = false;
    bool seen_arch = false;
    bool seen_pedantic = false;
    bool seen_march_native = false;
    bool seen_mcpu_native = false;
    bool seen_mtune_native = false;
    std::string seen_parallel_flto;
    const char *standard = nullptr;
    // if rewriting includes and precompiling on remote machine, then cpp args are not local
    Argument_Type Arg_Cpp = compiler_only_rewrite_includes(job) ? Arg_Rest : Arg_Local;

    explicit_color_diagnostics = false;
    explicit_no_show_caret = false;

    for (int i = had_cc ? 2 : 1; argv[i]; i++) {
        const char *a = argv[i];

        if (icerun) {
            args.append(a, Arg_Local);
        } else if (a[0] == '-') {
            if (!strcmp(a, "-E")) {
                always_local = true;
                args.append(a, Arg_Local);
                log_warning() << "preprocessing, building locally" << endl;
            } else if (!strncmp(a, "-fdump", 6)
                       || !strcmp(a, "-combine")
                       || !strcmp(a, "-fsyntax-only")
                       || !strncmp(a, "-ftime-report", strlen("-ftime-report"))
                       || !strcmp(a, "-ftime-trace")) {
                always_local = true;
                args.append(a, Arg_Local);
                log_warning() << "argument " << a << ", building locally" << endl;
            } else if (!strcmp(a, "-MD") || !strcmp(a, "-MMD") ||
                       str_startswith("-Wp,-MD", a) ||
                       str_startswith("-Wp,-MMD", a)) {
                seen_md = true;
                args.append(a, Arg_Local);
                /* These two generate dependencies as a side effect.  They
                 * should work with the way we call cpp. */
            } else if (!strcmp(a, "-MG") || !strcmp(a, "-MP")) {
                args.append(a, Arg_Local);
                /* These just modify the behaviour of other -M* options and do
                 * nothing by themselves. */
            } else if (!strcmp(a, "-MF") || str_startswith("-Wp,-MF", a)) {
                seen_mf = true;
                args.append(a, Arg_Local);
                args.append(argv[++i], Arg_Local);
                /* as above but with extra argument */
            } else if (!strcmp(a, "-MT") || !strcmp(a, "-MQ") ||
                       str_startswith("-Wp,-MT", a) ||
                       str_startswith("-Wp,-MQ", a)) {
                args.append(a, Arg_Local);
                args.append(argv[++i], Arg_Local);
                /* as above but with extra argument */
            } else if (a[1] == 'M') {
                /* -M(anything else) causes the preprocessor to
                   produce a list of make-style dependencies on
                   header files, either to stdout or to a local file.
                   It implies -E, so only the preprocessor is run,
                   not the compiler.  There would be no point trying
                   to distribute it even if we could. */
                always_local = true;
                args.append(a, Arg_Local);
                log_warning() << "argument " << a << ", building locally" << endl;
            } else if (str_equal("--param", a)) {
                args.append(a, Arg_Remote);

                assert( is_argument_with_space( a ));
                /* skip next word, being option argument */
                if (argv[i + 1]) {
                    args.append(argv[++i], Arg_Remote);
                }
            } else if (a[1] == 'B') {
                /* -B overwrites the path where the compiler finds the assembler.
                   As we don't use that, better force local job.
                */
                always_local = true;
                args.append(a, Arg_Local);
                log_warning() << "argument " << a << ", building locally" << endl;

                if (str_equal(a, "-B")) {
                    assert( is_argument_with_space( a ));
                    /* skip next word, being option argument */
                    if (argv[i + 1]) {
                        args.append(argv[++i], Arg_Local);
                    }
                }
            } else if (str_startswith("-Wa,", a)) {
                /* The -Wa option specifies a list of arguments
                 * that are passed to the assembler.
                 * We split them into individual arguments and
                 * call analyze_assembler_arg() for each one.
                 */
                const char *pos = a + 4, *next_comma;
                bool local = false;
                string as_arg;
                string remote_arg = "-Wa";

                while (1) {
                    next_comma = strchr(pos, ',');

                    if (next_comma)
                        as_arg.assign(pos, next_comma - pos);
                    else
                        as_arg = pos;

                    local = analyze_assembler_arg(as_arg, extrafiles);
                    remote_arg += "," + as_arg;

                    if (!next_comma)
                        break;

                    pos = next_comma + 1;
                }

                if (local) {
                    always_local = true;
                    args.append(a, Arg_Local);
                    log_warning() << "argument " << a << ", building locally" << endl;
                } else {
                    args.append(remote_arg, Arg_Remote);
                }
            } else if (!strcmp(a, "-S")) {
                seen_s = true;
            } else if (!strcmp(a, "-fprofile-arcs")
                       || !strcmp(a, "-ftest-coverage")
                       || !strcmp(a, "--coverage")
                       || !strcmp(a, "-frepo")
                       || !strcmp(a, "-fprofile-generate")
                       || !strcmp(a, "-fprofile-use")
                       || !strcmp(a, "-save-temps")
                       || !strcmp(a, "--save-temps")
                       || str_startswith(a, "-save-temps=")
                       || str_startswith(a, "--save-temps=")
                       || !strcmp(a, "-fbranch-probabilities")) {
                log_warning() << "compiler will emit additional local files (argument " << a << "); building locally" << endl;
                always_local = true;
                args.append(a, Arg_Local);
            } else if (!strcmp(a, "-gsplit-dwarf")) {
                args.append(a, Arg_Rest);
                seen_split_dwarf = true;
            } else if (str_equal(a, "-x")) {
                args.append(a, Arg_Rest);
                bool unsupported = true;
                std::string unsupported_opt = "??";
                if (const char *opt = argv[i + 1]) {
                    ++i;
                    args.append(opt, Arg_Rest);
                    unsupported_opt = opt;
                    if (str_equal(opt, "c++") || str_equal(opt, "c") || str_equal(opt, "objective-c") || str_equal(opt, "objective-c++")) {
                        CompileJob::Language lang = CompileJob::Lang_Custom;
                        if( str_equal(opt, "c")) {
                            lang = CompileJob::Lang_C;
                        } else if( str_equal(opt, "c++")) {
                            lang = CompileJob::Lang_CXX;
                        } else if( str_equal(opt, "objective-c")) {
                            lang = CompileJob::Lang_OBJC;
                        } else if( str_equal(opt, "objective-c++")) {
                            lang = CompileJob::Lang_OBJCXX;
                        } else {
                            continue;
                        }
                        job.setLanguage(lang); // will cause -x used remotely twice, but shouldn't be a problem
                        unsupported = false;
                    }
                }
                if (unsupported) {
                    log_warning() << "unsupported -x option: " << unsupported_opt << "; running locally" << endl;
                    always_local = true;
                }
            } else if (!strcmp(a, "-march=native")) {
                args.append(a, Arg_Rest);
                seen_march_native = true;
            } else if (!strcmp(a, "-mcpu=native")) {
                args.append(a, Arg_Rest);
                seen_mcpu_native = true;
            } else if (!strcmp(a, "-mtune=native")) {
                args.append(a, Arg_Rest);
                seen_mtune_native = true;
            } else if (!strcmp(a, "-fexec-charset") || !strcmp(a, "-fwide-exec-charset") || !strcmp(a, "-finput-charset") ) {
                log_warning() << "-f*-charset assumes charset conversion in the build environment; must be local" << endl;
                always_local = true;
                args.append(a, Arg_Local);
            } else if (!strcmp(a, "-c")) {
                seen_c = true;
            } else if (str_startswith("-o", a)) {
                if (!strcmp(a, "-o")) {
                    /* Whatever follows must be the output */
                    if (argv[i + 1]) {
                        ofile = argv[++i];
                    }
                } else {
                    a += 2;
                    ofile = a;
                }

                if (ofile == "-") {
                    /* Different compilers may treat "-o -" as either "write to
                     * stdout", or "write to a file called '-'".  We can't know,
                     * so we just always run it locally.  Hopefully this is a
                     * pretty rare case. */
                    log_warning() << "output to stdout?  running locally" << endl;
                    always_local = true;
                }
            } else if (str_equal("-D", a) || str_equal("-U", a)) {
                args.append(a, Arg_Cpp);

                assert( is_argument_with_space( a ));
                /* skip next word, being option argument */
                if (argv[i + 1]) {
                    ++i;
                    args.append(argv[i], Arg_Cpp);
                }
            } else if (str_equal("-I", a)
                       || str_equal("-i", a)
                       || str_equal("--include-directory", a)
                       || str_equal("-L", a)
                       || str_equal("-l", a)
                       || str_equal("--library-directory", a)
                       || str_equal("-MF", a)
                       || str_equal("-MT", a)
                       || str_equal("-MQ", a)
                       || str_equal("-cxx-isystem", a)
                       || str_equal("-c-isystem", a)
                       || str_equal("-idirafter", a)
                       || str_equal("--include-directory-after", a)
                       || str_equal("-iframework", a)
                       || str_equal("-iframeworkwithsysroot", a)
                       || str_equal("-imacros", a)
                       || str_equal("-imultilib", a)
                       || str_equal("-iprefix", a)
                       || str_equal("--include-prefix", a)
                       || str_equal("-iquote", a)
                       || str_equal("-include", a)
                       || str_equal("-include-pch", a)
                       || str_equal("-isysroot", a)
                       || str_equal("-isystem", a)
                       || str_equal("-isystem-after", a)
                       || str_equal("-ivfsoverlay", a)
                       || str_equal("-iwithprefix", a)
                       || str_equal("--include-with-prefix", a)
                       || str_equal("--include-with-prefix-after", a)
                       || str_equal("-iwithprefixbefore", a)
                       || str_equal("--include-with-prefix-before", a)
                       || str_equal("-iwithsysroot", a)) {
                args.append(a, Arg_Local);

                assert( is_argument_with_space( a ));
                /* skip next word, being option argument */
                if (argv[i + 1]) {
                    ++i;

                    if (str_startswith("-O", argv[i])) {
                        always_local = true;
                        log_warning() << "argument " << a << " " << argv[i] << ", building locally" << endl;
                    }

                    args.append(argv[i], Arg_Local);
                }
            } else if (str_equal("-fmodules", a)
                       || str_equal("-fcxx-modules", a)
                       || str_equal("-fmodules-ts", a)
                       || str_startswith("-fmodules-cache-path=", a)) {
                args.append(a, Arg_Local);
            } else if (str_startswith("-Wp,", a)
                       || str_startswith("-D", a)
                       || str_startswith("-U", a)) {
                args.append(a, Arg_Cpp);
            } else if (str_startswith("-I", a)
                       || str_startswith("-l", a)
                       || str_startswith("-L", a)) {
                args.append(a, Arg_Local);
            } else if (str_equal("-undef", a)) {
                args.append(a, Arg_Cpp);
            } else if (str_equal("-nostdinc", a)
                       || str_equal("-nostdinc++", a)
                       || str_equal("-MD", a)
                       || str_equal("-MMD", a)
                       || str_equal("-MG", a)
                       || str_equal("-MP", a)) {
                args.append(a, Arg_Local);
            } else if (str_equal("-Wmissing-include-dirs", a)
                       || str_equal("-Werror=missing-include-dirs", a)) {
                args.append(a, Arg_Local);
            } else if (str_equal("-fno-color-diagnostics", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fcolor-diagnostics", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fno-diagnostics-color", a)
                       || str_equal("-fdiagnostics-color=never", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fdiagnostics-color", a)
                       || str_equal("-fdiagnostics-color=always", a)) {
                explicit_color_diagnostics = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fdiagnostics-color=auto", a)) {
                // Drop the option here and pretend it wasn't given,
                // the code below will decide whether to enable colors or not.
                explicit_color_diagnostics = false;
            } else if (str_equal("-fno-diagnostics-show-caret", a)) {
                explicit_no_show_caret = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-fdiagnostics-show-caret", a)) {
                explicit_no_show_caret = false;
                args.append(a, Arg_Rest);
            } else if (str_startswith("-fplugin=", a)
                       || str_startswith("-fsanitize-blacklist=", a)
                       || str_startswith("-fprofile-sample-use=", a)
                       || str_startswith("-fprofile-instr-use=", a)) {
                const char* prefix = nullptr;
                static const char* const prefixes[] = { "-fplugin=", "-fsanitize-blacklist=",
                    "-fprofile-sample-use=", "-fprofile-instr-use=" };
                for(const char* const pr : prefixes) {
                    if( str_startswith(pr, a)) {
                        prefix = pr;
                        break;
                    }
                }
                assert( prefix != NULL );
                string file = a + strlen(prefix);

                if (access(file.c_str(), R_OK) == 0) {
                    file = get_absfilename(file);
                    extrafiles->push_back(file);
                } else {
                    always_local = true;
                    log_warning() << "file for argument " << a << " missing, building locally" << endl;
                }

                args.append(prefix + file, Arg_Rest);
            } else if (str_equal("-Xclang", a)) {
                if (argv[i + 1]) {
                    ++i;
                    const char *p = argv[i];

                    if (str_equal("-load", p)) {
                        if (argv[i + 1] && argv[i + 2] && str_equal(argv[i + 1], "-Xclang")) {
                            args.append(a, Arg_Rest);
                            args.append(p, Arg_Rest);
                            string file = argv[i + 2];

                            if (access(file.c_str(), R_OK) == 0) {
                                file = get_absfilename(file);
                                extrafiles->push_back(file);
                            } else {
                                always_local = true;
                                log_warning() << "plugin for argument "
                                           << a << " " << p << " " << argv[i + 1] << " " << file
                                           << " missing, building locally" << endl;
                            }

                            args.append(argv[i + 1], Arg_Rest);
                            args.append(file, Arg_Rest);
                            i += 2;
                        }
                    } else if( str_equal( "-building-pch-with-obj", p )) {
                        // We strip the arguments loading the PCH when building remotely,
                        // so the object file would not contain anything from the PCH,
                        // leading to link errors later when other objects would assume
                        // this object file would provide some symbols from the PCH.
                        log_info() << "argument " << a << " " << p << ", building locally" << endl;
                        always_local = true;
                        args.append(a, Arg_Rest);
                        args.append(p, Arg_Rest);
                    } else {
                        args.append(a, Arg_Rest);
                        args.append(p, Arg_Rest);
                    }
                }
            } else if (str_equal("-target", a)) {
                seen_target = true;
                args.append(a, Arg_Rest);
                if (argv[i + 1]) {
                    args.append(argv[++i], Arg_Rest);
                }
            } else if (str_startswith("--target=", a)) {
                seen_target = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-Wunused-macros", a)
                       || str_equal("-Werror=unused-macros", a)) {
                wunused_macros = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-Wno-unused-macros", a)) {
                wunused_macros = false;
                args.append(a, Arg_Rest);
            } else if (str_equal("-pedantic", a)
                       || str_equal("-Wpedantic", a)
                       || str_equal("-pedantic-errors", a)) {
                seen_pedantic = true;
                args.append(a, Arg_Rest);
            } else if (str_equal("-arch", a)) {
                if( seen_arch ) {
                    log_warning() << "multiple -arch options, building locally" << endl;
                    always_local = true;
                }
                seen_arch = false;
                args.append(a, Arg_Rest);
                if (argv[i + 1]) {
                    args.append(argv[++i], Arg_Rest);
                }
            } else if (str_startswith("-flto", a)) {
                args.append(a, Arg_Rest);
                // If -flto will be parallel (and thus use all cores), make it a "full" job
                // so that it reserves the entire local node. The only non-parallel -flto
                // options appear to be GCC's -flto without arguments or -flto=1.
                if( compiler_is_clang(job))
                    seen_parallel_flto = a;
                else if( !str_equal("-flto", a) && !str_equal("-flto=1", a))
                    seen_parallel_flto = a;
            } else {
                args.append(a, Arg_Rest);

                if (is_argument_with_space(a)) {
                    if (argv[i + 1]) {
                        args.append(argv[++i], Arg_Rest);
                    }
                }
            }
        } else if (a[0] == '@') {
            args.append(a, Arg_Local);
        } else {
            args.append(a, Arg_Rest);
        }
    }

    if (!seen_c && !seen_s) {
        if (!always_local) {
            log_warning() << "neither -c nor -S argument, building locally" << endl;
        }
        always_local = true;
    } else if (seen_s) {
        if (seen_c) {
            log_info() << "can't have both -c and -S, ignoring -c" << endl;
        }

        args.append("-S", Arg_Remote);
    } else {
        assert( seen_c );
        args.append("-c", Arg_Remote);
        if (seen_split_dwarf) {
            job.setDwarfFissionEnabled(true);
        }
    }

    if (!always_local) {
        ArgumentsList backup = args;

        /* TODO: ccache has the heuristic of ignoring arguments that are not
         * extant files when looking for the input file; that's possibly
         * worthwile.  Of course we can't do that on the server. */
        string ifile;

        for (ArgumentsList::iterator it = args.begin(); it != args.end();) {
            if (it->first == "-") {
                always_local = true;
                log_warning() << "stdin/stdout argument, building locally" << endl;
                break;
            }

            // Skip compiler arguments which are followed by another
            // argument not starting with -.
            if (it->first == "-Xclang" || it->first == "-x" || is_argument_with_space(it->first.c_str())) {
                ++it;
                ++it;
            } else if (it->second != Arg_Rest || it->first.at(0) == '-'
                       || it->first.at(0) == '@') {
                ++it;
            } else if (ifile.empty()) {
#if CLIENT_DEBUG
                log_info() << "input file: " << it->first << endl;
#endif
                job.setInputFile(it->first);
                ifile = it->first;
                it = args.erase(it);
                if (should_always_build_locally(ifile)) {
                    log_info() << "configure tests are run locally: "
                               << ifile << endl;
                    always_local = true;
                }
            } else {
                log_warning() << "found another non option on command line. Two input files? "
                           << it->first << endl;
                always_local = true;
                args = backup;
                job.setInputFile(string());
                break;
            }
        }

        if (ifile.find('.') != string::npos) {
            string::size_type dot_index = ifile.rfind('.');
            string ext = ifile.substr(dot_index + 1);

            if (ext == "cc"
                || ext == "cpp" || ext == "cxx"
                || ext == "cp" || ext == "c++"
                || ext == "C" || ext == "ii") {
#if CLIENT_DEBUG

                if (job.language() != CompileJob::Lang_CXX) {
                    log_info() << "switching to C++ for " << ifile << endl;
                }

#endif
                job.setLanguage(CompileJob::Lang_CXX);
            } else if (ext == "mi" || ext == "m") {
                job.setLanguage(CompileJob::Lang_OBJC);
            } else if (ext == "mii" || ext == "mm"
                       || ext == "M") {
                job.setLanguage(CompileJob::Lang_OBJCXX);
            } else if (ext == "s" || ext == "S" // assembler
                       || ext == "ads" || ext == "adb" // ada
                       || ext == "f" || ext == "for" // fortran
                       || ext == "FOR" || ext == "F"
                       || ext == "fpp" || ext == "FPP"
                       || ext == "r")  {
                always_local = true;
                log_info() << "source file " << ifile << ", building locally" << endl;
            } else if (ext != "c" && ext != "i") {   // C is special, it depends on arg[0] name
                log_warning() << "unknown extension " << ext << endl;
                always_local = true;
            }

            if (!always_local && ofile.empty()) {
                ofile = ifile.substr(0, dot_index);

                if (seen_s) {
                    ofile += ".s";
                } else {
                    ofile += ".o";
                }
                ofile = find_basename(ofile);
            }

            if (!always_local && seen_md && !seen_mf) {
                string dfile = ofile.substr(0, ofile.rfind('.')) + ".d";

#if CLIENT_DEBUG
                log_info() << "dep file: " << dfile << endl;
#endif

                args.append("-MF", Arg_Local);
                args.append(dfile, Arg_Local);
            }
        }

    } else { // always_local
        job.setInputFile(string());
    }

    struct stat st;

    if( !always_local ) {
        if (ofile.empty() || (!stat(ofile.c_str(), &st) && !S_ISREG(st.st_mode))) {
            log_warning() << "output file empty or not a regular file, building locally" << endl;
            always_local = true;
        }
    }

    // redirecting compiler's output will turn off its automatic coloring, so force it
    // when it would be used, unless explicitly set
    if (!icerun && compiler_has_color_output(job) && !explicit_color_diagnostics) {
        if (compiler_is_clang(job))
            args.append("-fcolor-diagnostics", Arg_Rest);
        else
            args.append("-fdiagnostics-color", Arg_Rest); // GCC
    }

    // -Wunused-macros is tricky with remote preprocessing. GCC's -fdirectives-only outright
    // refuses to work if -Wunused-macros is given, and Clang's -frewrite-includes is buggy
    // if -Wunused-macros is used (https://bugs.llvm.org/show_bug.cgi?id=15614). It's a question
    // if this could possibly even work, given that macros may be used as filenames for #include directives.
    if( wunused_macros ) {
        job.setBlockRewriteIncludes(true);
    }

    // -pedantic doesn't work with remote preprocessing, if extensions to a named standard
    // are allowed.  GCC allows GNU extensions by default, so let's check if a standard
    // other than eg gnu11 or gnu++14 was specified.
    if( seen_pedantic && !compiler_is_clang(job) && (!standard || str_startswith("gnu", standard)) ) {
        log_warning() << "argument -pedantic, forcing local preprocessing (try using -std=cXX instead of -std=gnuXX)" << endl;
        job.setBlockRewriteIncludes(true);
    }

    if( !always_local && compiler_only_rewrite_includes(job) && !compiler_is_clang(job)) {
        // Inject this, so that remote compilation uses -fpreprocessed -fdirectives-only
        args.append("-fdirectives-only", Arg_Remote);
    }

    if( !always_local && !seen_target && compiler_is_clang(job)) {
        // With gcc each binary can compile only for one target, so cross-compiling is just
        // a matter of using the proper cross-compiler remotely and it will automatically
        // compile for the given platform. However one clang binary can compile for many
        // platforms and so when cross-compiling it would by default compile for the remote
        // host platform. Therefore explicitly ask for our platform.
        string default_target = clang_get_default_target(job);
        if( !default_target.empty()) {
            args.append("-target", Arg_Remote);
            args.append(default_target, Arg_Remote);
        } else {
            always_local = true;
            log_error() << "failed to read default clang host platform, building locally" << endl;
        }
    }
    if( !always_local && (seen_march_native || seen_mcpu_native || seen_mtune_native)) {
        // These depend on the host where the compilation is done, so expand these
        // to the actual flags that would be used if compiled locally.
        list<string> arch_args;
        if(compiler_get_arch_flags(job, seen_march_native, seen_mcpu_native, seen_mtune_native, arch_args)) {
            bool handled = false;
            for( auto it = args.begin(); it != args.end();) {
                if( it->first == "-march=native" || it->first == "-mcpu=native" || it->first == "-mtune=native" ) {
                    // Remove all the arguments, replace the first one with the specific flags.
                    it = args.erase( it );
                    if( !handled ) {
                        for( const string& arg : arch_args ) {
                            it = args.insert( it, std::make_pair( arg, Arg_Rest ));
                            ++it;
                        }
                        handled = true;
                    }
                } else {
                    ++it;
                }
            }
            assert( handled );
        }
        else {
            always_local = true;
            log_error() << "failed to read -march/cpu/tune=native flags, building locally" << endl;
        }
    }

    job.setFlags(args);
    job.setOutputFile(ofile);

#if CLIENT_DEBUG
    trace() << "scanned result: local args=" << concat_args(job.localFlags())
            << ", remote args=" << concat_args(job.remoteFlags())
            << ", rest=" << concat_args(job.restFlags())
            << ", local=" << always_local
            << ", compiler=" << job.compilerName()
            << ", lang=" << job.language()
            << endl;
#endif

    int ret = 0;
    if( always_local ) {
        ret |= AlwaysLocal;
        if( !seen_parallel_flto.empty() && !seen_c ) {
            ret |= FullJob;
            trace() << seen_parallel_flto << " and no -c, building with all local slots" << endl;
        }
    }
    return ret;
}
