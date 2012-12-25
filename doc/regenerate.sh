STYLESHEET=$(kde4-config --path data --locate ksgmltools2/customization/kde-include-man.xsl)
MEINPROC=meinproc4 --stylesheet $(STYLESHEET)
$(MEINPROC) man-icecc.1.docbook
$(MEINPROC) man-icecc-scheduler.1.docbook
$(MEINPROC) man-iceccd.1.docbook
$(MEINPROC) man-icecream.7.docbook
