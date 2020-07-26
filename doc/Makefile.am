if WITH_ICECREAM_MAN

man_verbose = $(man_verbose_@AM_V@)
man_verbose_ = $(man_verbose_@AM_DEFAULT_V@)
man_verbose_0 = @echo MAN $@;

# do not show anything here
xml_verbose = $(xml_verbose_@AM_V@)
xml_verbose_ = $(xml_verbose_@AM_DEFAULT_V@)
xml_verbose_0 = @

%.xml: $(srcdir)/%.adoc
	$(xml_verbose)$(ASCIIDOC) -a revnumber=${VERSION} -d manpage -b docbook -o $(builddir)/$@ $<

%.1: %.xml
	$(man_verbose)$(A2X) --doctype manpage --format manpage $<

%.7: %.xml
	$(man_verbose)$(A2X) --doctype manpage --format manpage $<

dist_man_MANS = \
	icecc.1 \
	icerun.1 \
	iceccd.1 \
	icecc-scheduler.1 \
	icecc-create-env.1 \
	icecream.7

CLEANFILES = $(dist_man_MANS)

endif

EXTRA_DIST = \
	icecc.adoc \
	icerun.adoc \
	iceccd.adoc \
	icecc-scheduler.adoc \
	icecc-create-env.adoc \
	icecream.adoc
