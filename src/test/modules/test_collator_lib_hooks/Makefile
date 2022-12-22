# src/test/modules/test_collator_lib_hooks/Makefile

MODULE_big = test_collator_lib_hooks
OBJS = \
	$(WIN32RES) \
	test_collator_lib_hooks.o test_icu_hook.o
PGFILEDESC = "test_collator_lib_hooks - test collator provider library hooks"

EXTENSION = test_collator_lib_hooks

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_collator_lib_hooks
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

export with_icu
