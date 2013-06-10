if WITH_ICECREAM_MAN

icecc.1: $(srcdir)/man-icecc.1.xml
	$(DOCBOOK2X) $<

icecc-create-env.1: $(srcdir)/man-icecc-create-env.1.xml
	$(DOCBOOK2X) $<

icecc-scheduler.1: $(srcdir)/man-icecc-scheduler.1.xml
	$(DOCBOOK2X) $<

iceccd.1: $(srcdir)/man-iceccd.1.xml
	$(DOCBOOK2X) $<

icecream.7: $(srcdir)/man-icecream.7.xml
	$(DOCBOOK2X) $<

dist_man_MANS = \
	icecc.1 \
	iceccd.1 \
	icecc-scheduler.1 \
	icecc-create-env.1 \
	icecream.7

endif

EXTRA_DIST = \
	man-icecc.1.xml \
	man-iceccd.1.xml \
	man-icecc-scheduler.1.xml \
	man-icecc-create-env.1.xml \
	man-icecream.7.xml
