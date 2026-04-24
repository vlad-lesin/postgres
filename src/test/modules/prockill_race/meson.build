# Copyright (c) 2025-2026, PostgreSQL Global Development Group

if not get_option('injection_points')
  subdir_done()
endif

prockill_race_sources = files('prockill_race.c')

if host_system == 'windows'
  prockill_race_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'prockill_race',
    '--FILEDESC', 'prockill_race - ProcKill lock-group test helpers',])
endif

prockill_race = shared_module('prockill_race',
  prockill_race_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += prockill_race

test_install_data += files(
  'prockill_race.control',
  'prockill_race--1.0.sql',
)

tests += {
  'name': 'prockill_race',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
      'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_prockill_lockgroup_injection.pl',
    ],
  },
}
