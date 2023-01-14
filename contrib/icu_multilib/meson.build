# Copyright (c) 2022-2023, PostgreSQL Global Development Group

icu_multilib_sources = files(
  'icu_multilib.c',
)

if host_system == 'windows'
  icu_multilib_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'icu_multilib',
    '--FILEDESC', 'icu_multilib -- support multiple ICU library versions',])
endif

icu_multilib = shared_module('icu_multilib',
  icu_multilib_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += icu_multilib

install_data(
  'icu_multilib.control',
  'icu_multilib--1.0.sql',
  kwargs: contrib_data_args,
)

