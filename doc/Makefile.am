STYLESHEET=$(shell kde4-config --path data --locate ksgmltools2/customization/kde-include-man.xsl)
MEINPROC=meinproc4 --stylesheet $(STYLESHEET)
icecc.1: $(srcdir)/man-icecc.1.docbook
	$(MEINPROC) $(srcdir)/man-icecc.1.docbook 

scheduler.1: $(srcdir)/man-scheduler.1.docbook
	$(MEINPROC) $(srcdir)/man-scheduler.1.docbook 

iceccd.1: $(srcdir)/man-iceccd.1.docbook
	$(MEINPROC) $(srcdir)/man-iceccd.1.docbook 

icecream.7: $(srcdir)/man-icecream.7.docbook
	$(MEINPROC) $(srcdir)/man-icecream.7.docbook

dist_man_MANS = icecc.1 iceccd.1 icecream.7 scheduler.1

EXTRA_DIST = man-icecc.1.docbook man-iceccd.1.docbook man-icecream.7.docbook man-scheduler.1.docbook
