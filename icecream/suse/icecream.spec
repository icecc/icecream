#
# spec file for package icecream (Version 0.1)
#
# Copyright (c) 2004 SUSE LINUX AG, Nuernberg, Germany.
# This file and all modifications and additions to the pristine
# package are under the same license as the package itself.
#
# Please submit bugfixes or comments via http://www.suse.de/feedback/
#

# norootforbuild
# neededforbuild  gcc-c++ libstdc++-devel

BuildRequires: aaa_base acl attr bash bind-utils bison bzip2 coreutils cpio cpp cracklib cvs cyrus-sasl db devs diffutils e2fsprogs file filesystem fillup findutils flex gawk gdbm-devel glibc glibc-devel glibc-locale gpm grep groff gzip info insserv kbd less libacl libattr libgcc libselinux libstdc++ libxcrypt libzio m4 make man mktemp module-init-tools ncurses ncurses-devel net-tools netcfg openldap2-client openssl pam pam-modules patch permissions popt procinfo procps psmisc pwdutils rcs readline sed strace syslogd sysvinit tar tcpd texinfo timezone unzip util-linux vim zlib zlib-devel autoconf automake binutils gcc gcc-c++ gdbm gettext libstdc++-devel libtool perl rpm

Name:         icecream
License:      GPL, LGPL
Group:        Development/Tools/Building
Summary:      for distributed compile in the network
Requires:     /bin/tar /usr/bin/bzip2
PreReq:       %fillup_prereq
Version:      0.1
Release:      10
Source0:      %name-%{version}.tar.bz2
Source1:      init.icecream
Source2:      sysconfig.icecream
Patch0:       disable-monitor.diff
BuildRoot:    %{_tmppath}/%{name}-%{version}-build

%description
icecream is the next generation distcc



Authors:
--------
    Stephan Kulow <coolo@suse.de>
    Michael Matz <matz@suse.de>
    Cornelius Schumacher <cschum@suse.de>
    Lubos Lunak <llunak@suse.cz>
    Frerich Raabe <raabe@kde.org>

%prep
%setup -q -n %name
%patch0 -p0

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
mkdir -p $RPM_BUILD_ROOT%{_sbindir}
mv $RPM_BUILD_ROOT%{_bindir}/iceccd $RPM_BUILD_ROOT%{_sbindir}
mkdir -p $RPM_BUILD_ROOT/opt/icecream/bin
ln -s /usr/bin/icecc $RPM_BUILD_ROOT/opt/icecream/bin/g++
ln -s /usr/bin/icecc $RPM_BUILD_ROOT/opt/icecream/bin/gcc
#
# Install icecream init script
mkdir -p $RPM_BUILD_ROOT/etc/init.d/
install -m 755 %SOURCE1 $RPM_BUILD_ROOT/etc/init.d/icecream
ln -sf /etc/init.d/icecream $RPM_BUILD_ROOT%{_sbindir}/rcicecream
mkdir -p $RPM_BUILD_ROOT/var/adm/fillup-templates
install -m 644 %SOURCE2 $RPM_BUILD_ROOT/var/adm/fillup-templates/sysconfig.icecream

%preun
%stop_on_removal icecream

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
%_bindir/create-env
%_sbindir/iceccd
%_sbindir/rcicecream
%_sbindir/scheduler
/opt/icecream
/var/adm/fillup-templates/sysconfig.icecream

%changelog -n icecream
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
