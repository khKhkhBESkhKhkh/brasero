SUBDIRS = icons mime

convertdir=$(datadir)/GConf/gsettings
convert_DATA=brasero.convert

gsettings_in_file = org.gnome.brasero.gschema.xml.in
gsettings_SCHEMAS = $(gsettings_in_file:.xml.in=.xml)
@INTLTOOL_XML_NOMERGE_RULE@
@GSETTINGS_RULES@

desktopdir=$(datadir)/applications
desktop_in_files =	brasero.desktop.in
desktop_DATA = $(desktop_in_files:.desktop.in=.desktop)

@INTLTOOL_DESKTOP_RULE@

CLEANFILES =		$(desktop_DATA)		\
			$(gsettings_SCHEMAS)


EXTRA_DIST = 		$(desktop_in_files)	\
			$(gsettings_in_file)	\
			$(convert_DATA)

UPDATE_DESKTOP = \
	if [ -f $(DESTDIR)$(datadir)/applications/defaults.list ] ; then \
		if which update-desktop-database>/dev/null 2>&1 ; then \
			update-desktop-database; \
		fi \
	fi

install-data-hook: 
if UPDATE_CACHES
	$(UPDATE_DESKTOP)
else
	@true
endif

uninstall-hook: 
if UPDATE_CACHES
	$(UPDATE_DESKTOP)
else
	@true
endif

-include $(top_srcdir)/git.mk
