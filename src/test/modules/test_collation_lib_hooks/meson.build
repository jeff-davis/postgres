# FIXME: prevent install during main install, but not during test :/

test_collation_lib_hooks_sources = files(
  'test_collation_lib_hooks.c',
  'test_libc_hook.c',
  'test_icu_hook.c',
)

if host_system == 'windows'
  test_collation_lib_hooks_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_collation_lib_hooks',
    '--FILEDESC', 'test_collation_lib_hooks - test collation provider library hooks',])
endif

test_collation_lib_hooks = shared_module('test_collation_lib_hooks',
  test_collation_lib_hooks_sources,
  kwargs: pg_mod_args,
)
testprep_targets += test_collation_lib_hooks

install_data(
  'test_collation_lib_hooks.control',
  kwargs: contrib_data_args,
)

have_lc_version_mask = cc.has_header_symbol('locale.h', 'LC_VERSION_MASK')
glibc = cc.has_header_symbol('features.h', '__GLIBC__')

tests += {
  'name': 'test_collation_lib_hooks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_icu.pl',
      't/002_libc.pl',
      't/003_libc_mixed_collation.pl',
      't/004_libc_version.pl',
    ],
    'env': {
      'with_icu': icu.found() ? 'yes' : 'no',
      'win32': (host_system == 'windows') ? 'yes' : 'no',
      'glibc': glibc ? 'yes' : 'no',
      'have_lc_version_mask': have_lc_version_mask ? 'yes' : 'no',
    },
  },
}
