EXTRA_DIST = icecream.spec.in \
	init.icecream \
	logrotate \
	SuSEfirewall.iceccd \
	SuSEfirewall.scheduler \
	sysconfig.icecream

initdir=$(sysconfdir)/init.d
templatesdir=$(localstatedir)/adm/fillup-templates
icecreamcachedir=$(localstatedir)/cache/icecream
logdir=$(localstatedir)/log/icecream
logrotatedir=$(sysconfdir)/logrotate.d
susefirewallservicesdir=$(sysconfdir)/sysconfig/SuSEfirewall2.d/services

install: init.icecream logrotate SuSEfirewall.iceccd SuSEfirewall.scheduler sysconfig.icecream
	if test $(host_vendor) = "suse" -o $(host_vendor) = "ibm" ; then \
	  mkdir -p $(DESTDIR)/$(initdir) ;\
	  $(INSTALL) -m 755 $(srcdir)/init.icecream $(DESTDIR)$(initdir)/icecream ;\
	  ln -sf $(initdir)/icecream $(DESTDIR)$(sbindir)/rcicecream ;\
	  mkdir -p $(DESTDIR)$(templatesdir) ;\
	  $(INSTALL) -m 644 $(srcdir)/sysconfig.icecream $(DESTDIR)$(templatesdir)/sysconfig.icecream ;\
	  mkdir -p $(DESTDIR)$(icecreamcachedir) ;\
	  mkdir -p $(DESTDIR)$(logdir) ;\
	  $(INSTALL) -m 644 -D $(srcdir)/logrotate $(DESTDIR)$(logrotatedir)/icecream ;\
	  mkdir -p $(DESTDIR)$(susefirewallservicesdir) ;\
	  $(INSTALL) -m 644 $(srcdir)/SuSEfirewall.iceccd $(DESTDIR)$(susefirewallservicesdir)/iceccd ;\
	  $(INSTALL) -m 644 $(srcdir)/SuSEfirewall.scheduler $(DESTDIR)$(susefirewallservicesdir)/icecream-scheduler ;\
	fi

uninstall:
	if test $(host_vendor) = "suse"; then \
	  rm $(DESTDIR)$(initdir)/icecream ;\
	  rmdir $(DESTDIR)/$(initdir) ;\
	  rm $(DESTDIR)$(sbindir)/rcicecream ;\
	  rm $(DESTDIR)$(templatesdir)/sysconfig.icecream ;\
	  rmdir $(DESTDIR)$(templatesdir) ;\
	  rmdir $(DESTDIR)$(icecreamcachedir) ;\
	  rmdir $(DESTDIR)$(logdir) ;\
	  rm $(DESTDIR)$(logrotatedir)/icecream ;\
	  rm $(DESTDIR)$(susefirewallservicesdir)/iceccd ;\
	  rm $(DESTDIR)$(susefirewallservicesdir)/icecream-scheduler ;\
	  rmdir $(DESTDIR)$(susefirewallservicesdir) ;\
	fi

