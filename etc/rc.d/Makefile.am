initdir = $(DEFAULT_INIT_DIR)
init_SCRIPTS = zfsd

EXTRA_DIST = \
	$(top_srcdir)/etc/rc.d/zfsd.in

$(init_SCRIPTS):%:%.in Makefile
	-($(SED) -e 's,@initdir\@,$(initdir),g' \
	     $< >'$@'; \
	     chmod +x '$@')

CLEANFILES = $(init_SCRIPTS)
