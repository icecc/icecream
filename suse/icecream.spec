#
# spec file for package icecream (Version 0.6.3)
#
# Copyright (c) 2006 SUSE LINUX Products GmbH, Nuernberg, Germany.
# This file and all modifications and additions to the pristine
# package are under the same license as the package itself.
#
# Please submit bugfixes or comments via http://bugs.opensuse.org/
#

# norootforbuild
# don't use icecc to avoid bootstrap problems
# icecream 0

Name:           icecream
BuildRequires:  gcc-c++
License:        GPL, LGPL
Group:          Development/Tools/Building
Summary:        For Distributed Compile in the Network
Requires:       /bin/tar /usr/bin/bzip2
PreReq:         %fillup_prereq
Prereq:         /usr/sbin/useradd /usr/sbin/groupadd
Requires:       gcc-c++
Version:        0.6.3
Release:        5
Source0:        ftp://ftp.suse.com/pub/projects/icecream/%name-%{version}.tar.bz2
BuildRoot:      %{_tmppath}/%{name}-%{version}-build

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
rm -r mon

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
for i in g++ gcc cc c++; do 
  ln -s /usr/bin/icecc $RPM_BUILD_ROOT/opt/icecream/bin/$i
  rm -f $RPM_BUILD_ROOT/usr/bin/$i
done
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
