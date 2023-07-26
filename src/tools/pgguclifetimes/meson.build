# Copyright (c) 2022-2023, PostgreSQL Global Development Group

pgguclifetimes_sources = files(
  'pgguclifetimes.c',
  'vector.c'
)

libclang_dep = dependency(
    'Clang',
    method: 'cmake',
    modules: ['libclang'],
    required: false
)
if not libclang_dep.found()
    libclang_dep = cc.find_library('clang')
endif

if host_system == 'windows'
  pgguclifetimes_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgguclifetimes',
    '--FILEDESC', 'pgguclifetimes - check GUC lifetime annotations'])
endif

pgguclifetimes = executable('pgguclifetimes',
  pgguclifetimes_sources,
  dependencies: [frontend_code, libclang_dep],
  include_directories: include_directories(
  		       '.',
  		       '/usr/lib/llvm-16/include/', # FIXME: how to find his properly?
  ),
  kwargs: default_bin_args + {
    'install': false,
# possibly at some point do this:
#   'install_dir': dir_pgxs / 'src/tools/pgguclifetimes',
  },
)
bin_targets += pgguclifetimes

tests += {
  'name': 'pgguclifetimes',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_pgguclifetimes.pl',
    ],
  },
}
