#
# spec file for package icecream (Version 0.6)
#
# Copyright (c) 2005 SUSE LINUX Products GmbH, Nuernberg, Germany.
# This file and all modifications and additions to the pristine
# package are under the same license as the package itself.
#
# Please submit bugfixes or comments via http://www.suse.de/feedback/
#

# norootforbuild
# neededforbuild  gcc-c++ libstdc++-devel

BuildRequires: aaa_base acl attr bash bind-utils bison bzip2 coreutils cpio cpp cracklib cvs cyrus-sasl db devs diffutils e2fsprogs file filesystem fillup findutils flex gawk gdbm-devel gettext-devel glibc glibc-devel glibc-locale gpm grep groff gzip info insserv klogd less libacl libattr libcom_err libgcc libnscd libselinux libstdc++ libxcrypt libzio m4 make man mktemp module-init-tools ncurses ncurses-devel net-tools netcfg openldap2-client openssl pam pam-modules patch permissions popt procinfo procps psmisc pwdutils rcs readline sed strace sysvinit tar tcpd texinfo timezone unzip util-linux vim zlib zlib-devel autoconf automake binutils gcc gcc-c++ gdbm gettext libstdc++-devel libtool perl rpm

Name:         icecream
License:      GPL, LGPL
Group:        Development/Tools/Building
Summary:      For Distributed Compile in the Network
Requires:     /bin/tar /usr/bin/bzip2
PreReq:       %fillup_prereq
Prereq:       /usr/sbin/useradd /usr/sbin/groupadd
Requires:     gcc-c++
Version:      0.6.1
Release:      1
Source0:      ftp://ftp.suse.com/pub/projects/icecream/%name-%{version}.tar.bz2
BuildRoot:    %{_tmppath}/%{name}-%{version}-build

%description
icecream is the next generation distcc.



Authors:
--------
    Stephan Kulow <coolo@suse.de>
    Michael Matz <matz@suse.de>
    Cornelius Schumacher <cschum@suse.de>
    Lubos Lunak <llunak@suse.cz>
    Frerich Raabe <raabe@kde.org>

%prep
%setup -q -n %name

%build
export CFLAGS="$RPM_OPT_FLAGS"
export CXXFLAGS="$RPM_OPT_FLAGS"
make -f Makefile.cvs
./configure \
  --prefix=%_prefix \
  --mandir=%_mandir 
make

%install
rm -rf $RPM_BUILD_ROOT
make DESTDIR=$RPM_BUILD_ROOT KDEDIR=/opt/kde3 install
mkdir -p $RPM_BUILD_ROOT/opt/icecream/bin
ln -s /usr/bin/icecc $RPM_BUILD_ROOT/opt/icecream/bin/g++
ln -s /usr/bin/icecc $RPM_BUILD_ROOT/opt/icecream/bin/gcc
ln -s /usr/bin/icecc $RPM_BUILD_ROOT/opt/icecream/bin/cc
ln -s /usr/bin/icecc $RPM_BUILD_ROOT/opt/icecream/bin/c++
#
# Install icecream init script
mkdir -p $RPM_BUILD_ROOT/etc/init.d/
install -m 755 suse/init.icecream $RPM_BUILD_ROOT/etc/init.d/icecream
ln -sf /etc/init.d/icecream $RPM_BUILD_ROOT%{_sbindir}/rcicecream
mkdir -p $RPM_BUILD_ROOT/var/adm/fillup-templates
install -m 644 suse/sysconfig.icecream $RPM_BUILD_ROOT/var/adm/fillup-templates/sysconfig.icecream
mkdir -p $RPM_BUILD_ROOT/var/cache/icecream

%preun
%stop_on_removal icecream

%pre
/usr/sbin/groupadd -r icecream 2> /dev/null || :
/usr/sbin/useradd -r -g icecream -s /bin/false -c "Icecream Daemon" -d /var/cache/icecream icecream 2> /dev/null || :

%post
%{fillup_and_insserv -n icecream icecream}

%postun
%restart_on_update icecream
%{insserv_cleanup}

%clean
rm -rf ${RPM_BUILD_ROOT}

%files
%defattr(-,root,root)
/etc/init.d/icecream
%_bindir/icecc
%_sbindir/scheduler
%_bindir/create-env
%_sbindir/iceccd
%_sbindir/rcicecream
/opt/icecream
/var/adm/fillup-templates/sysconfig.icecream
%attr(-,icecream,icecream) /var/cache/icecream

%changelog -n icecream
* Fri Sep 02 2005 - schwab@suse.de
- Require gcc-c++.
* Wed Apr 13 2005 - coolo@suse.de
- some changes to the daemon to keep the cache size below 100MB
* Wed Apr 13 2005 - coolo@suse.de
- update tarball
* Mon Feb 21 2005 - schwab@suse.de
- create-env: try to find generic versions of libraries.  Remove
  LD_ASSUME_KERNEL hack.
* Sat Feb 05 2005 - schwab@suse.de
- Don't set LD_ASSUME_KERNEL in BETA.
* Fri Jan 21 2005 - coolo@suse.de
- some fixes from CVS
* Mon Jan 17 2005 - schwab@suse.de
- create-env: Add specs only if it exists as file.
* Wed Nov 17 2004 - coolo@suse.de
- fixing dead loop
* Sun Nov 14 2004 - schwab@suse.de
- Don't use icecc during build.
* Tue Nov 02 2004 - coolo@suse.de
- ignore duplicated platforms to avoid confusion between native
  compiler and cross compiler
* Wed Oct 13 2004 - coolo@suse.de
- several improvements in the communication layer
- the daemon kills compiler jobs when the client exists before
  awaiting the result (gcc4 feature :)
* Tue Sep 28 2004 - od@suse.de
- in create-env, use LD_ASSUME_KERNEL=2.4.21 on ppc64
* Fri Sep 10 2004 - schwab@suse.de
- Workaround cfg bug in gcc.
* Mon Sep 06 2004 - coolo@suse.de
- handle being called without _any_ environment variables correctly
  (blender's use of scons)
* Mon Sep 06 2004 - coolo@suse.de
- correctly calculating output filename for -S jobs (grub's configure)
* Sun Sep 05 2004 - coolo@suse.de
- several improvements in the network code to make things more
  robust on general network slowness
- speed up configure runs
* Tue Aug 31 2004 - coolo@suse.de
- do calculate the load a bit more fair for those machines that got
  other niced jobs
- add time information to the log output
- track a bit more carefully the child pids
* Mon Aug 30 2004 - coolo@suse.de
- do not crash when the network goes down (again)
- some cleanup
* Sun Aug 29 2004 - coolo@suse.de
- finding quite some scheduler troubles while watching a network
  with half the computers using WLAN (KDE conference)
- run everything the daemon does with client data as specific user
- changed the spec file to create that user and move the cache
  dir to /var/cache/icecream
- more options for the sysconfig
* Wed Aug 18 2004 - coolo@suse.de
- avoid crashes when the connection between client and daemon
  goes down (as happend at night)
* Tue Aug 17 2004 - coolo@suse.de
- fix handling of unknown paramters (failed/gsl)
* Sun Aug 15 2004 - coolo@suse.de
- bugfixes and more flag statistics
* Thu Aug 12 2004 - coolo@suse.de
- transfer debug and optimization flags to the scheduler for better
  speed calculation
* Thu Aug 12 2004 - coolo@suse.de
- fixing ugly regression in the daemon. Increased protocol version
  to avoid problems with these old daemons
* Wed Aug 11 2004 - coolo@suse.de
- Fixing grave performance problem and several scheduler crashes
* Wed Aug 04 2004 - coolo@suse.de
- update for new automake, let the daemon set a ulimit for memory usage
* Fri Jul 30 2004 - coolo@suse.de
- don't stress the scheduler while compiling jobs three times
- also use icecream for .c files
- fix for the init script
* Tue Jun 29 2004 - coolo@suse.de
- fixing bugs reported by prague office and ro
* Fri Jun 11 2004 - coolo@suse.de
- major update (including fix for gcc build)
* Tue May 11 2004 - coolo@suse.de
- really fixing build with several input files
* Tue May 04 2004 - coolo@suse.de
- fix build with several input files (ltp package)
* Mon May 03 2004 - coolo@suse.de
- support cross compiling
* Wed Apr 28 2004 - coolo@suse.de
- support multiple architectures in the scheduler
* Mon Apr 26 2004 - coolo@suse.de
- filter out more errors as info message trying to get binutils's
  testsuite to work
* Sun Apr 25 2004 - coolo@suse.de
- adding -frandom-seed to the compilation for the jobs that compile
  thee times on several hosts
* Fri Apr 23 2004 - coolo@suse.de
- fixing grave bug in the setup of the protocol version which caused
  lookups
* Thu Apr 22 2004 - coolo@suse.de
- several fixes in the transport layer and the client now compiles
  every 5th job three times to test the farm
* Sat Apr 17 2004 - coolo@suse.de
- splitting monitor into an extra source to simplify build
  requirements for the client
* Fri Apr 16 2004 - coolo@suse.de
- new protocol version for fancier monitors
* Fri Apr 16 2004 - coolo@suse.de
- new version with revised monitor and new init script name
* Tue Apr 13 2004 - coolo@suse.de
- initial package
