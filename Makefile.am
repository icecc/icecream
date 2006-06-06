
SUBDIRS = minilzo services daemon client
AUTOMAKE_OPTIONS = foreign 1.6

.PHONY: changes
changes:
	sed -e "s#  *#:'#; s#\$$#>'#;" $$HOME/prod/kde-common/accounts | sed -e 's/  *\([^ ]*\)$$/ <\1/' > Changes.users
	cvs2cl.pl -I "CVS_SILENT.*" -U Changes.users
	rm Changes.users
