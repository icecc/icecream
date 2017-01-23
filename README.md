[Icecream](Icecream) was created by SUSE based on distcc. Like distcc,
[Icecream](Icecream) takes compile jobs from a build and
distributes it among remote machines allowing a parallel build. But
unlike distcc, [Icecream](Icecream) uses a central server that
dynamically schedules the compile jobs to the fastest free server. This
advantage pays off mostly for shared computers, if you're the only user
on x machines, you have full control over them.

Table of Contents

-   [Installation](#Installation)
-   [How to use icecream](#How_to_use_icecream)
    -   [make it persistent](#make_it_persistent)

-   [TroubleShooting](#TroubleShooting)
    -   [Firewall](#Firewall)
    -   [C compiler](#C_compiler)
    -   [osc build](#osc_build)
    -   [some compilation node aren't
        used](#some_compilation_node_arent_used)

-   [Supported platforms](#Supported_platforms)
-   [Using icecream in heterogeneous
    environments](#Using_icecream_in_heterogeneous_environments)
-   [Cross-Compiling using icecream](#CrossCompiling_using_icecream)
-   [Creating cross compiler package](#Creating_cross_compiler_package)
-   [Cross-Compiling for embedded targets using
    icecream](#CrossCompiling_for_embedded_targets_using_icecream)
-   [How to combine icecream with
    ccache](#How_to_combine_icecream_with_ccache)
-   [Debug output](#Debug_output)
-   [Some Numbers](#Some_Numbers)
-   [What is the best environment for
    icecream](#What_is_the_best_environment_for_icecream)
-   [Network setup for Icecream
    (firewalls)](#Network_setup_for_Icecream_firewalls)
-   [I use distcc, why should I
    change?](#I_use_distcc_why_should_I_change)
-   [I use teambuilder, why should I
    change?](#I_use_teambuilder_why_should_I_change)
-   [Icecream on gentoo](#Icecream_on_gentoo)
-   [Bug tracker](#Bug_tracker)
-   [Repository](#Repository)

Installation
-------------------------------------------------------------------------

To install icecream, issue

     yast -i icecream icecream-monitor

In case this should not work, here are some links to download Icecream:

-   [Binaries from
    opensuse.org](http://software.opensuse.org/download/home:/coolo/)
-   [Sources from
    ftp.suse.com](ftp://ftp.suse.com/pub/projects/icecream)
-   [openSUSE Icecream node
    LiveCD](http://forgeftp.novell.com/kiwi-ltsp/icecream/)

If you have questions not answered here, feel free to contact
icecream-users@googlegroups.com  (icecream-users+subscribe@googlegroups.com).

How to use icecream
---------------------------------------------------------------------------------------

You need:

-   One machine that runs the scheduler ("./icecc-scheduler -d")
-   Many machines that run the daemon ("./iceccd -d")

It is possible to run the scheduler and the daemon on one machine and
only the daemon on another, thus forming a compile cluster with two
nodes.

If you want to compile using icecream, make sure \$prefix/lib/icecc/bin is the
first first entry in your path, e.g. type

     export PATH=/usr/lib/icecc/bin:$PATH

(Hint: put this in \~/.bashrc or /etc/profile to not have to type it in
everytime)

Then you just compile with make -j \<num\>, where
\<num\> is the amount of jobs you want to compile in parallel.
As a start, take the number of logical processors multiplied with 2. But
note that numbers \>15 normally cause trouble. Here is an example:

     make -j6

WARNING: Never use icecream in untrusted environments. Run the daemons
and the scheduler as unprivileged user in such networks if you have to!
But you will have to rely on homogeneous networks then (see below).

If you want funny stats, you might want to run "icemon".

### make it persistent

If you restart a computer, you still want it to be in the icecream
cluster after reboot. With the SUSE packages, this is easy to
accomplish, just set the service to be started on boot:

     chkconfig icecream on

You can verify if the icecream service is running like this:

     /etc/init.d/icecream status
     Checking for Distributed Compiler Daemon:                             running

TroubleShooting
-------------------------------------------------------------------------------

Most problems are caused by firewalls and by make using the wrong C
compiler (e.g. /usr/bin/gcc instead of /usr/lib/icecc/bin/gcc).

### Firewall

For testing purposes, you can stop your firewall like this:

     rcSuSEfirewall2 stop

To open the right ports in your firewall, call

     yast2 firewall

Choose "allowed services" -\> Advanced. Enter for TCP: **10245 8765
8766** and for UDP **8765**

If you have scheduler running on another system, you should open
broadcasting response :

     yast2 firewall

Choose "Custom Rules" -\> Add. Enter Source Networ **0/0** Protocol:
**UDP** Source Port **8765**

### C compiler

To make sure your compile job uses /usr/lib/icecc/bin/gcc (gcc is used as
an example here, depending on your compile job it can also be g++, cc or
c++) start your compile using

     make VERBOSE=1

and wait for a typical compile command to appear, like this one:

     cd /root/kdepim/kode/libkode && /usr/lib/icecc/bin/c++  -DTest1Area=5121 -D_BSD_SOURCE 
     -D_XOPEN_SOURCE=500 -D_BSD_SOURCE -DQT_NO_STL 
     -DQT_NO_CAST_TO_ASCII -D_REENTRANT -DKDE_DEPRECATED_WARNINGS 
     -DKDE_DEFAULT_DEBUG_AREA=5295 -DMAKE_KODE_LIB -Wnon-
     virtual-dtor -Wno-long-long -ansi -Wundef -Wcast-align 
     -Wchar-subscripts-Wall -W -Wpointer-arith -Wformat-security 
     -fno-exceptions -fno-check-new

in this example, the right c compiler is chosen, /usr/lib/icecc/bin/c++.
If the wrong one is chosen, delete CMakeCache.txt (if existing) and
start the build process again calling ./configure (if existing).

### osc build

You can tell osc build to use icecream to build packages, by appending
--icecream=\<n\> where n is the number of process which should be
started in parallel. However, for integration with icecream to work
properly, you must install icecream on the host where you will run "osc
build" and you must start icecream daemon.

### some compilation node aren't used

If, when using icecream monitor (icemon), you notice some nodes not
being used at all for compilation, check you have the same icecream
version on all nodes, otherwise, nodes running older icecream version
might be excluded from available nodes.

The icecream version shipped with openSUSE 12.2 is partially incompatible
with nodes using other icecream versions. 12.2 nodes will not be used for compilation
by other nodes, and depending on the scheduler version 12.2 nodes will not compile
on other nodes either. These incompatible nodes can be identified by having 
'Linux3_' prefix in the platform). Replace the openSUSE 12.2 package
with a different one (for example from the devel:tools:build repository).

Supported platforms
---------------------------------------------------------------------------------------

Most of icecream is UNIX specific and can be used on most platforms, but
as the scheduler needs to know the load of a machine, there are some
tricky parts. Supported are:

      - Linux
      - FreeBSD
      - DragonFlyBSD
      - OS X

Note that all these platforms can be used both as server and as client -
meaning you can do full cross compiling between them.

Using icecream in heterogeneous environments
-----------------------------------------------------------------------------------------------------------------------------------------

If you are running icecream daemons (note: they _all_ must be running
as root. In the future icecream might gain the ability to know when
machines can't accept a different env, but for now it is all or nothing
) in the same icecream network but on machines with incompatible
compiler versions you have to tell icecream which environment you are
using. Use

      icecc --build-native

to create an archive file containing all the files necessary to setup
the compiler environment. The file will have a random unique name like
"ddaea39ca1a7c88522b185eca04da2d8.tar.bz2" per default. Rename it to
something more expressive for your convenience, e.g.
"i386-3.3.1.tar.bz2". Set

      ICECC_VERSION=<filename_of_archive_containing_your_environment>

in the shell environment where you start the compile jobs and the file
will be transferred to the daemons where your compile jobs run and
installed to a chroot environment for executing the compile jobs in the
environment fitting to the environment of the client. This requires that
the icecream daemon runs as root.

If you do not set ICECC\_VERSION, the client will use a tar ball
provided by the daemon running on the same machine. So you can always be
sure you're not tricked by incompatible gcc versions - and you can share
your computer with users of other distributions (or different versions
of your beloved SUSE Linux :)

Cross-Compiling using icecream
------------------------------------------------------------------------------------------------------------

SUSE got quite some good machines not having a processor from Intel or
AMD, so icecream is pretty good in using cross-compiler environments
similar to the above way of spreading compilers. There the
ICECC\_VERSION variable looks like \<native\_filename\>(,\<platform\>:\<cross\_compiler\_filename\>)\*,
for example like this:

     /work/9.1-i386.tar.bz2,ia64:/work/9.1-cross-ia64.tar.bz2,Darwin_PowerPCMac:/work/osx-generate-i386.tar.gz

To get this working on openSuse machines there are some packages
containing the cross-compiler environments. Here is a sample case
showing how to do to get it working. Let's assume that we want to build
for x86\_64 but use some i386 machines for the build as well. On the
x86\_64 machine, go to
[http://software.opensuse.org,](http://software.opensuse.org) search
for **icecream x86\_64** and download and install the version for i586.
Then add this to the ICECC\_VERSION and build.

     i386:/usr/share/icecream-envs/cross-x86_64-gcc-icecream-backend_i386.tar.gz

Creating cross compiler package
---------------------------------------------------------------------------------------------------------------

How to package such a cross compiler is pretty straightforward if you
look what's inside the tarballs generated by icecc. You basically need a
/usr/bin/gcc, a /usr/bin/g++ and a /usr/bin/as. So if you need a cross
compiler that uses your OS X running G5 to compile i586-linux for your
laptop, you would:

-   go to your OS X and download binutils and gcc (of the versions you
    use on linux)
-   first compile and install binutils with --prefix /usr/local/cross
    --target=i586-linux (I have some problems that required setting CC
    and AR)
-   configure gcc with the same options, go into the gcc directory and
    make all install-driver install-common - that worked good enough for
    me.
-   now create a new directory where you copy
    /usr/local/cross/bin/i586-linux-{gcc,g++,as} into as
    usr/bin/{gcc,g++,as}
-   now I copy an empty.c (that is empty) into that dir too and call

      chroot . usr/bin/gcc -c empty.c

that will report an error about missing libraries or missing cc1 - copy
them until gcc generates an empty.o without error. You can double check
with "file empty.o" if it's really a i586-linux object file.

-   now tar that directory and use it on your client as specified above.

My cross compiler for the above case is under
[http://ktown.kde.org/\~coolo/ppc-osx-create-i586.tar.gz](http://ktown.kde.org/~coolo/ppc-osx-create-i586.tar.gz)

Cross-Compiling for embedded targets using icecream
------------------------------------------------------------------------------------------------------------------------------------------------------

When building for embedded targets like ARM often you'll have a
toolchain that runs on your host and produces code for the target. In
these situations you can exploit the power of icecream as well.

Create symbolic links from where icecc is to the name of your cross
compilers (e.g. arm-linux-g++ and arm-linux-gcc), make sure that these
symbolic links are in the path and before the path of your toolchain,
with $ICECC\_CC and $ICECC\_CXX you need to tell icecream which
compilers to use for preprocessing and local compiling. e.g. set it to
ICECC\_CC=arm-linux-gcc and ICECC\_CXX=arm-linux-g++.

As the next step you need to create a .tar.bz2 of your cross compiler,
check the result of icecc --build-native to see what needs to be
present.

Finally one needs to set ICECC\_VERSION and point it to the tar.bz2
you've created. When you start compiling your toolchain will be used.

NOTE: with ICECC\_VERSION you point out on which platforms your
toolchain runs, you do not indicate for which target code will be
generated.

How to combine icecream with ccache
-----------------------------------------------------------------------------------------------------------------------

The easiest way to use ccache with icecream is to set CCACHE\_PREFIX to
icecc (the actual icecream client wrapper):

     export CCACHE_PREFIX=icecc

This will make ccache prefix any compilation command it needs to do with
icecc, making it use icecream for the compilation (but not for
preprocessing alone).

To actually use ccache, the mechanism is the same like with using
icecream alone. Since ccache does not provide any symlinks in
/opt/ccache/bin, you can create them manually:

     mkdir /opt/ccache/bin
     ln -s /usr/bin/ccache /opt/ccache/bin/gcc
     ln -s /usr/bin/ccache /opt/ccache/bin/g++

And then compile with

     export PATH=/opt/ccache/bin:$PATH

Note however that ccache isn't really worth the trouble if you're not
recompiling your project three times a day from scratch (it adds some
overhead in comparing the source files and uses quite some disk space).

Debug output
-------------------------------------------------------------------------

You can use the environment variable ICECC\_DEBUG to control if icecream
gives debug output or not. Set it to "debug" to get debug output. The
other possible values are error, warning and info (the -v option for
daemon and scheduler raise the level per -v on the command line - so use
-vvv for full debug).

Some Numbers
-------------------------------------------------------------------------

Numbers of my test case (some STL C++ genetic algorithm)

-   g++ on my machine: 1.6s
-   g++ on fast machine: 1.1s
-   icecream using my machine as remote machine: 1.9s
-   icecream using fast machine: 1.8s

The icecream overhead is quite huge as you might notice, but the
compiler can't interleave preprocessing with compilation and the file
needs to be read/written once more and in between the file is
transferred.

But even if the other computer is faster, using g++ on my local machine
is faster. If you're (for whatever reason) alone in your network at some
point, you loose all advantages of distributed compiling and only add
the overhead. So icecream got a special case for local compilations (the
same special meaning that localhost got within \$DISTCC\_HOSTS). This
makes compiling on my machine using icecream down to 1.7s (the overhead
is actually less than 0.1s in average).

As the scheduler is aware of that meaning, it will prefer your own
computer if it's free and got not less than 70% of the fastest available
computer.

Keep in mind, that this affects only the first compile job, the second
one is distributed anyway. So if I had to compile two of my files, I
would get

-   g++ -j1 on my machine: 3.2s
-   g++ -j1 on the fast machine: 2.2s
-   using icecream -j2 on my machine: max(1.7,1.8)=1.8s
-   (using icecream -j2 on the other machine: max(1.1,1.8)=1.8s)

The math is a bit tricky and depends a lot on the current state of the
compilation network, but make sure you're not blindly assuming make -j2
halves your compilation time.

What is the best environment for icecream
-----------------------------------------------------------------------------------------------------------------------------------

In most requirements icecream isn't special, e.g. it doesn't matter what
distributed compile system you use, you won't have fun if your nodes are
connected through than less or equal to 10MBit. Note that icecream
compresses input and output files (using lzo), so you can calculate with
\~1MBit per compile job - i.e more than make -j10 won't be possible
without delays.

Remember that more machines are only good if you can use massive
parallelism, but you will for sure get the best result if your
submitting machine (the one you called g++ on) will be fast enough to
feed the others. Especially if your project consists of many easy to
compile files, the preprocessing and file IO will be job enough to need
a quick machine.

The scheduler will try to give you the fastest machines available, so
even if you add old machines, they will be used only in exceptional
situations, but still you can have bad luck - the scheduler doesn't know
how long a job will take before it started. So if you have 3 machines
and two quick to compile and one long to compile source file, you're not
safe from a choice where everyone has to wait on the slow machine. Keep
that in mind.

Network setup for Icecream (firewalls)
---------------------------------------------------------------------------------------------------------------------------

A short overview of the ports icecream requires:

-   TCP/10245 on the daemon computers (required)
-   TCP/8765 for the the scheduler computer (required)
-   TCP/8766 for the telnet interface to the scheduler (optional)
-   UDP/8765 for broadcast to find the scheduler (optional)

Note that the [SuSEfirewall2](SuSEfirewall2) on SUSE \< 9.1 got some
problems configuring broadcast. So you might need the -s option for the
daemon in any case there. If the monitor can't find the scheduler, use
USE\_SCHEDULER=\<host\> icemon (or send me a patch :)

I use distcc, why should I change?
-------------------------------------------------------------------------------------------------------------------

If you're sitting alone home and use your partner's computer to speed up
your compilation and both these machines run the same Linux version,
you're fine with distcc (as 95% of the users reading this chapter will
be, I'm sure). But there are several situations, where distcc isn't the
best choice:

-   you're changing compiler versions often and still want to speed up
    your compilation (see the ICECC\_VERSION support)
-   you got some neat PPC laptop and want to use your wife's computer
    that only runs intel (see the cross compiler section)
-   you don't know what machines will be on-line at compile time.
-   **most important**: you're sitting in a office with several
    co-workers that do not like if you overload their workstations when
    they play doom (distcc doesn't have a scheduler)
-   you like nice compile monitors :)

I use teambuilder, why should I change?
-----------------------------------------------------------------------------------------------------------------------------

That part is easier: your evaluation license runs out soon. But even if
that isn't true: The teambuilder documentation says that it doesn't
require any system change whatsoever to setup. But this is only true in
homogeneous networks. If you want to test out the latest gcc CVS you
must either appear pretty convincing to your co-workers or you're on
your own and can watch the teambuilder monitor playing for the others.

Icecream on gentoo
-------------------------------------------------------------------------------------

-   It is recommended to remove all processor specific optimizations
    from the CFLAGS line in /etc/make.conf. On the aKademy cluster it
    proved useful to use only "-O2", otherwise there are often internal
    compiler errors, if not all computers have the same processor
    type/version

**Be aware** that you have to change the CFLAGS during ich gcc update
too.

-   To use icecream with emerge/ebuild use PREROOTPATH=/opt/icecream/lib/icecc/bin
    emerge bla
-   Be aware, because your gcc/glibc/binutils are normally compiled with
    processor-specific flags, there is a high chance that your compiler
    won't work on other machines. The best would be to build gcc, glibc
    and binutils without those flags and copying the needed files into
    your tarball for distribution, e.g. CFLAGS="-mcpu=i686 -O3
    -fomit-frame-pointer -pipe" CXXFLAGS="$CFLAGS" ebuild
    /usr/portage/sys-devel/gcc-yourver.ebuild install ; cp
    /var/tmp/portage...

Bug tracker
-----------------------------------------------------------------------

Create a github issue on https://github.com/icecc/icecream

Repository
---------------------------------------------------------------------

The git repository lives at https://github.com/icecc/icecream
