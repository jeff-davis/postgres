# FIXME: prevent install during main install, but not during test :/

test_collator_lib_hooks_sources = files(
  'test_collator_lib_hooks.c',
  'test_icu_hook.c',
)

if host_system == 'windows'
  test_collator_lib_hooks_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_collator_lib_hooks',
    '--FILEDESC', 'test_collator_lib_hooks - test collation provider library hooks',])
endif

test_collator_lib_hooks = shared_module('test_collator_lib_hooks',
  test_collator_lib_hooks_sources,
  kwargs: pg_mod_args,
)
testprep_targets += test_collator_lib_hooks

install_data(
  'test_collator_lib_hooks.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_collator_lib_hooks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_icu.pl',
    ],
    'env': {
      'with_icu': icu.found() ? 'yes' : 'no',
    },
  },
}
