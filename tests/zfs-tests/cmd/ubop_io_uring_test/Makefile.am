pkgexecdir = $(datadir)/@PACKAGE@/zfs-tests/bin

pkgexec_PROGRAMS = ubop_io_uring_test
ubop_io_uring_test_SOURCES = queue.c setup.c syscall.c ubop_io_uring_test.c
ubop_io_uring_test_LDADD = \
	$(abs_top_builddir)/lib/libspl/libspl.la \
