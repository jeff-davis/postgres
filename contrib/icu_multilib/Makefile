# contrib/icu_multilib/Makefile

MODULE_big = icu_multilib
OBJS = \
	$(WIN32RES) \
	icu_multilib.o

EXTENSION = icu_multilib
DATA = icu_multilib--1.0.sql
PGFILEDESC = "icu_multilib -- support multiple ICU library versions"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/icu_multilib
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
