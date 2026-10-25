project(
  'cvtebench',
  'c',
  version: '0.1.0',
  default_options: ['c_std=c17', 'warning_level=3'],
)

if host_machine.system() != 'linux'
  error('cvtebench supports modern Linux systems only')
endif

cc = meson.get_compiler('c')
if cc.get_define('__GLIBC__', prefix: '#include <features.h>') != ''
  error('cvtebench must be built with a musl-targeting compiler')
endif

math = cc.find_library('m', required: true)

inc = include_directories('include', 'vendor/tomlc17')
add_project_arguments(
  '-DCVTEBENCH_DATA_DIR="' + meson.current_source_dir() + '/data"',
  language: 'c',
)
sources = files(
  'src/main.c',
  'src/config.c',
  'src/buffer.c',
  'src/metrics.c',
  'src/worker.c',
  'src/controller.c',
  'src/benchmarks.c',
  'src/benchmarks/startup.c',
  'src/benchmarks/cursor_motion.c',
  'src/benchmarks/dense_cells.c',
  'src/benchmarks/light_cells.c',
  'src/benchmarks/medium_cells.c',
  'src/benchmarks/scrolling.c',
  'src/benchmarks/scrolling_bottom_region.c',
  'src/benchmarks/scrolling_bottom_small_region.c',
  'src/benchmarks/scrolling_fullscreen.c',
  'src/benchmarks/scrolling_top_region.c',
  'src/benchmarks/scrolling_top_small_region.c',
  'src/benchmarks/sync_medium_cells.c',
  'src/benchmarks/unicode.c',
  'src/benchmarks/missing.c',
  'vendor/tomlc17/tomlc17.c',
)

executable(
  'cvtebench',
  sources,
  include_directories: inc,
  dependencies: [math],
  link_args: ['-static'],
  install: false,
)
