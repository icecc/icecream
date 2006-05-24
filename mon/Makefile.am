SUBDIRS = doc

INCLUDES = -I$(srcdir)/.. $(all_includes)

bin_PROGRAMS = icemon
icemon_SOURCES = mon-kde.cc listview.cc ganttstatusview.cc \
                 summaryview.cc starview.cc
icemon_LDADD = libicemon_common.la ../services/libcommon.la $(LIB_KDEUI)
icemon_LDFLAGS = $(all_libraries)

noinst_LTLIBRARIES = libicemon_common.la
libicemon_common_la_SOURCES = job.cc monitor.cc hostinfo.cc statusview.cc \
			      detailedhostview.cc hostlistview.cc hostview.cc \
			      joblistview.cc

METASOURCES = AUTO

icedata_DATA = icemonui.rc
icedatadir = $(kde_datadir)/icemon

picsdir = $(kde_datadir)/icemon/pics
pics_DATA = icemonnode.png

kde_module_LTLIBRARIES = icemon_panelapplet.la

icemon_panelapplet_la_LDFLAGS = -avoid-version -module $(all_libraries)
icemon_panelapplet_la_LIBADD = libicemon_common.la ../services/libcommon.la $(LIB_KDEUI)
icemon_panelapplet_la_SOURCES = icemonapplet.cpp  

lnkdir = $(kde_datadir)/kicker/applets
lnk_DATA = icemonapplet.desktop

xdg_apps_DATA = icemon.desktop

EXTRA_DIST = $(xdg_apps_DATA)
