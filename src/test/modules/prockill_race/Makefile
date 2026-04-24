# src/test/modules/prockill_race/Makefile
#
# TAP test: concurrent ProcKill in a lock group must not put a PGPROC on the
# freelist while procLatch is still owned.  With the server fix, this test passes.

MODULE_big = prockill_race
OBJS = $(WIN32RES) prockill_race.o
PGFILEDESC = "prockill_race - ProcKill lock-group race test helpers"

EXTENSION = prockill_race
DATA = prockill_race--1.0.sql

TAP_TESTS = 1
NO_INSTALLCHECK = 1

# Temp-install: ensure injection_points and this module are installed.
REGRESS_PREP = prockill_race_tap_install

.PHONY: prockill_race_tap_install
prockill_race_tap_install:
	$(MAKE) -C '$(top_builddir)'/src/test/modules/injection_points DESTDIR='$(abs_top_builddir)'/tmp_install install
	$(MAKE) -C '$(top_builddir)'/src/test/modules/prockill_race DESTDIR='$(abs_top_builddir)'/tmp_install install

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/prockill_race
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

ifeq ($(enable_injection_points),yes)
ifndef NO_TEMP_INSTALL
prockill_race_tap_install: temp-install
endif
include $(top_srcdir)/contrib/contrib-global.mk
else
all install installdirs uninstall distprep:
	@echo "prockill_race TAP test requires --enable-injection-points"
check:
	@echo "prockill_race TAP test requires --enable-injection-points"
clean distclean maintainer-clean:
	@:
endif

endif
