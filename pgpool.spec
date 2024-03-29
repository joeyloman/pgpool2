# How to build RPM:
#   rpmbuild -ba pgpool.spec --define="pgpool_version 3.3.2" --define="pg_version 93" --define="pghome /usr/pgsql-9.3"
#
# expecting RPM name are:
#   pgpool-II-pg{xx}-{version}.pgdg.{arch}.rpm
#   pgpool-II-pg{xx}-devel-{version}.pgdg.{arch}.rpm
#   pgpool-II-pg{xx}-{version}.pgdg.src.rpm

Summary:        Pgpool is a connection pooling/replication server for PostgreSQL
Name:           pgpool-II-pg%{pg_version}
Version:        %{pgpool_version}
Release:        1%{?dist}
License:        BSD
Group:          Applications/Databases
Vendor:         Pgpool Global Development Group
URL:            http://www.pgppol.net/
Source0:        pgpool-II-%{version}.tar.gz
Source1:        pgpool.init
Source2:        pgpool.sysconfig
Patch1:         pgpool.conf.sample.patch
BuildRoot:      %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
BuildRequires:  postgresql%{pg_version}-devel pam-devel
Obsoletes:      postgresql-pgpool

# original pgpool archive name
%define archive_name pgpool-II-%{version}

%description
pgpool-II is a inherited project of pgpool (to classify from
pgpool-II, it is sometimes called as pgpool-I). For those of
you not familiar with pgpool-I, it is a multi-functional
middle ware for PostgreSQL that features connection pooling,
replication and load balancing functions. pgpool-I allows a
user to connect at most two PostgreSQL servers for higher
availability or for higher search performance compared to a
single PostgreSQL server.

pgpool-II, on the other hand, allows multiple PostgreSQL
servers (DB nodes) to be connected, which enables queries
to be executed simultaneously on all servers. In other words,
it enables "parallel query" processing. Also, pgpool-II can
be started as pgpool-I by changing configuration parameters.
pgpool-II that is executed in pgpool-I mode enables multiple
DB nodes to be connected, which was not possible in pgpool-I.

%package devel
Summary:     The development files for pgpool-II
Group:       Development/Libraries
Requires:    %{name} = %{version}

%description devel
Development headers and libraries for pgpool-II.

%prep
%setup -q -n %{archive_name}
%patch1 -p0

%build
%configure --with-pgsql-includedir=%{pghome}/include/ \
           --with-pgsql-libdir=%{pghome}/lib \
           --disable-static --with-pam --disable-rpath \
           --sysconfdir=%{_sysconfdir}/pgpool-II/

make %{?_smp_flags}

%install
rm -rf %{buildroot}
make %{?_smp_flags} DESTDIR=%{buildroot} install
install -d %{buildroot}%{_datadir}/pgpool-II
install -d %{buildroot}%{_sysconfdir}/pgpool-II
mv %{buildroot}/%{_sysconfdir}/pgpool-II/pcp.conf.sample %{buildroot}%{_sysconfdir}/pgpool-II/pcp.conf
mv %{buildroot}/%{_sysconfdir}/pgpool-II/pgpool.conf.sample %{buildroot}%{_sysconfdir}/pgpool-II/pgpool.conf
mv %{buildroot}/%{_sysconfdir}/pgpool-II/pool_hba.conf.sample %{buildroot}%{_sysconfdir}/pgpool-II/pool_hba.conf
install -d %{buildroot}%{_initrddir}
install -m 755 %{SOURCE1} %{buildroot}%{_initrddir}/pgpool
install -d %{buildroot}%{_sysconfdir}/sysconfig
install -m 644 %{SOURCE2} %{buildroot}%{_sysconfdir}/sysconfig/pgpool

# install to PostgreSQL
export PATH=%{pghome}/bin:$PATH
cd sql/pgpool-recovery/
make %{?_smp_flags} DESTDIR=%{buildroot} install
cd ../../
cd sql/pgpool-regclass/
make %{?_smp_flags} DESTDIR=%{buildroot} install
cd ../../

# nuke libtool archive and static lib
rm -f %{buildroot}%{_libdir}/libpcp.{a,la}

%clean
rm -rf %{buildroot}

%post
/sbin/ldconfig
chkconfig --add pgpool

%preun
if [ $1 = 0 ] ; then
    /sbin/service pgpool condstop >/dev/null 2>&1
    chkconfig --del pgpool
fi

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%dir %{_datadir}/pgpool-II
%doc README README.euc_jp TODO COPYING INSTALL AUTHORS ChangeLog NEWS doc/pgpool-en.html doc/pgpool-ja.html doc/pgpool.css doc/tutorial-en.html doc/tutorial-ja.html
%{_bindir}/pgpool
%{_bindir}/pcp_attach_node
%{_bindir}/pcp_detach_node
%{_bindir}/pcp_node_count
%{_bindir}/pcp_node_info
%{_bindir}/pcp_pool_status
%{_bindir}/pcp_proc_count
%{_bindir}/pcp_proc_info
%{_bindir}/pcp_promote_node
%{_bindir}/pcp_stop_pgpool
%{_bindir}/pcp_recovery_node
%{_bindir}/pcp_systemdb_info
%{_bindir}/pcp_watchdog_info
%{_bindir}/pg_md5
%{_mandir}/man8/pgpool*
%{_datadir}/pgpool-II/insert_lock.sql
%{_datadir}/pgpool-II/system_db.sql
%{_datadir}/pgpool-II/pgpool.pam
%{pghome}/share/extension/pgpool-recovery.sql
%{pghome}/share/extension/pgpool_recovery--1.0.sql
%{pghome}/share/extension/pgpool_recovery.control
%{pghome}/share/extension/pgpool-regclass.sql
%{pghome}/share/extension/pgpool_regclass--1.0.sql
%{pghome}/share/extension/pgpool_regclass.control
%{_sysconfdir}/pgpool-II/pgpool.conf.sample-master-slave
%{_sysconfdir}/pgpool-II/pgpool.conf.sample-replication
%{_sysconfdir}/pgpool-II/pgpool.conf.sample-stream
%{_libdir}/libpcp.so.*
%{pghome}/lib/pgpool-recovery.so
%{pghome}/lib/pgpool-regclass.so
%{_initrddir}/pgpool
%attr(764,root,root) %config(noreplace) %{_sysconfdir}/pgpool-II/*.conf
%config(noreplace) %{_sysconfdir}/sysconfig/pgpool

%files devel
%defattr(-,root,root,-)
%{_includedir}/libpcp_ext.h
%{_includedir}/pcp.h
%{_includedir}/pool_process_reporting.h
%{_includedir}/pool_type.h
%{_libdir}/libpcp.so

%changelog
* Tue Nov 26 2013 Nozomi Anzai <anzai@sraoss.co.jp> 3.3.1-1
- Improved to specify the versions of pgool-II and PostgreSQL

* Mon May 13 2013 Nozomi Anzai <anzai@sraoss.co.jp> 3.3.0-1
- Update to 3.3.0
- Change to install pgpool-recovery, pgpool-regclass to PostgreSQL

* Tue Nov 3 2009 Devrim Gunduz <devrim@CommandPrompt.com> 2.2.5-3
- Remove init script from all runlevels before uninstall. Per #RH Bugzilla
  532177

* Mon Oct 5 2009 Devrim Gunduz <devrim@CommandPrompt.com> 2.2.5-2
- Add 2 new docs, per Tatsuo.

* Sun Oct 4 2009 Devrim Gunduz <devrim@CommandPrompt.com> 2.2.5-1
- Update to 2.2.5, for various fixes described at
  http://lists.pgfoundry.org/pipermail/pgpool-general/2009-October/002188.html
- Re-apply a fix for Red Hat Bugzilla #442372

* Wed Sep 9 2009 Devrim Gunduz <devrim@CommandPrompt.com> 2.2.4-1
- Update to 2.2.4

* Wed May 6 2009 Devrim Gunduz <devrim@CommandPrompt.com> 2.2.2-1
- Update to 2.2.2

* Sun Mar 1 2009 Devrim Gunduz <devrim@CommandPrompt.com> 2.2-1
- Update to 2.2
- Fix URL
- Own /usr/share/pgpool-II directory.
- Fix pid file path in init script, per    pgcore #81.
- Fix spec file -- we don't use short_name macro in pgcore spec file.
- Create pgpool pid file directory, per pgcore #81.
- Fix stop/start routines, also improve init script a bit.
- Install conf files to a new directory (/etc/pgpool-II), and get rid
  of sample conf files.

* Fri Aug 8 2008 Devrim Gunduz <devrim@CommandPrompt.com> 2.1-1
- Update to 2.1
- Removed temp patch #4.

* Sun Jan 13 2008 Devrim Gunduz <devrim@CommandPrompt.com> 2.0.1-1
- Update to 2.0.1
- Add a temp patch that will disappear in 2.0.2

* Fri Oct 5 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.2.1-1
- Update to 1.2.1

* Wed Aug 29 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.2-5
- Chmod sysconfig/pgpool to 644, not 755. Per BZ review.
- Run chkconfig --add pgpool during %%post.

* Thu Aug 16 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.2-4
- Fixed the directory name where sample conf files and sql files
  are installed.

* Sun Aug 5 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.2-3
- Added a patch for sample conf file to use Fedora defaults

* Sun Aug 5 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.2-2
- Added an init script for pgpool
- Added /etc/sysconfig/pgpool

* Wed Aug 1 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.2-1
- Update to 1.2

* Fri Jun 15 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.1.1-1
- Update to 1.1.1

* Sat Jun 2 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.1-1
- Update to 1.1
- added --disable-rpath configure parameter.
- Chowned sample conf files, so that they can work with pgpoolAdmin.

* Thu Apr 22 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.0.2-4
- Added postgresql-devel as BR, per bugzilla review.
- Added --disable-static flan, per bugzilla review.
- Removed superfluous manual file installs, per bugzilla review.

* Thu Apr 22 2007 Devrim Gunduz <devrim@CommandPrompt.com> 1.0.2-3
- Rebuilt for the correct tarball
- Fixed man8 file ownership, per bugzilla review #229321

* Tue Feb 20 2007 Jarod Wilson <jwilson@redhat.com> 1.0.2-2
- Create proper devel package, drop -libs package
- Nuke rpath
- Don't install libtool archive and static lib
- Clean up %%configure line
- Use proper %%_smp_mflags
- Install config files properly, without .sample on the end
- Preserve timestamps on header files

* Tue Feb 20 2007 Devrim Gunduz <devrim@commandprompt.com> 1.0.2-1
- Update to 1.0.2-1

* Mon Oct 02 2006 Devrim Gunduz <devrim@commandprompt.com> 1.0.1-5
- Rebuilt

* Mon Oct 02 2006 Devrim Gunduz <devrim@commandprompt.com> 1.0.1-4
- Added -libs and RPM
- Fix .so link problem
- Cosmetic changes to spec file

* Thu Sep 27 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 1.0.1-3
- Fix spec, per Yoshiyuki Asaba

* Thu Sep 26 2006 - Devrim GUNDUZ <devrim@commandprompt.com> 1.0.1-2
- Fixed rpmlint errors
- Fixed download url
- Added ldconfig for .so files

* Thu Sep 21 2006 - David Fetter <david@fetter.org> 1.0.1-1
- Initial build pgpool-II 1.0.1 for PgPool Global Development Group
