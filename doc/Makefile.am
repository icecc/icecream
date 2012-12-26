STYLESHEET=$(shell kde4-config --path data --locate ksgmltools2/customization/kde-include-man.xsl)
MEINPROC=meinproc4 --stylesheet $(STYLESHEET)
icecc.1: $(srcdir)/man-icecc.1.docbook
	-$(MEINPROC) $(srcdir)/man-icecc.1.docbook

icecc-scheduler.1: $(srcdir)/man-icecc-scheduler.1.docbook
	-$(MEINPROC) $(srcdir)/man-icecc-scheduler.1.docbook

iceccd.1: $(srcdir)/man-iceccd.1.docbook
	-$(MEINPROC) $(srcdir)/man-iceccd.1.docbook

icecream.7: $(srcdir)/man-icecream.7.docbook
	-$(MEINPROC) $(srcdir)/man-icecream.7.docbook

dist_man_MANS = icecc.1 iceccd.1 icecc-scheduler.1 icecream.7

EXTRA_DIST = man-icecc.1.docbook man-iceccd.1.docbook man-icecc-scheduler.1.docbook man-icecream.7.docbook
