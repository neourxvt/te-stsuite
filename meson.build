project(
  'te-stsuite',
  'c',
  version: '0.1.0',
  meson_version: '>=1.3.0',
  default_options: ['c_std=c17', 'warning_level=3'],
)

if host_machine.system() != 'linux'
  error('te-stsuite supports modern Linux systems only')
endif

cc = meson.get_compiler('c')
if cc.get_define('__GLIBC__', prefix: '#include <features.h>') != ''
  error('te-stsuite must be built with a musl-targeting compiler')
endif

math = cc.find_library('m', required: true)

fs = import('fs')
source_relative_to_build = fs.relative_to(
  meson.current_source_dir(),
  meson.current_build_dir(),
)
reproducibility_args = [
  '-ffile-prefix-map=' + meson.current_build_dir() + '=/usr/src/te-stsuite/build',
  '-ffile-prefix-map=' + source_relative_to_build + '=/usr/src/te-stsuite',
  '-ffile-prefix-map=' + meson.current_source_dir() + '=/usr/src/te-stsuite',
  '-Werror=date-time',
]
foreach arg : reproducibility_args
  if not cc.has_argument(arg)
    error('the selected compiler must support reproducible build argument: ' + arg)
  endif
endforeach
add_project_arguments(reproducibility_args, language: 'c')

inc = include_directories('include', 'vendor/tomlc17', 'vendor/yyjson/src')
add_project_arguments(
  '-DYYJSON_DISABLE_READER=1',
  '-DYYJSON_DISABLE_INCR_READER=1',
  '-DYYJSON_DISABLE_UTILS=1',
  '-DYYJSON_DISABLE_FILE=1',
  '-DYYJSON_DISABLE_NON_STANDARD=1',
  language: 'c',
)
sources = files(
  'src/main.c',
  'src/config.c',
  'src/buffer.c',
  'src/data.c',
  'src/monitor.c',
  'src/monitors/polling.c',
  'src/monitors/cgroupv2.c',
  'src/worker.c',
  'src/process_isolation.c',
  'src/controller.c',
  'src/benchmarks.c',
  'src/benchmarks/startup.c',
  'src/benchmarks/vtebench_cursor_motion.c',
  'src/benchmarks/vtebench_dense_cells.c',
  'src/benchmarks/vtebench_light_cells.c',
  'src/benchmarks/vtebench_medium_cells.c',
  'src/benchmarks/vtebench_scrolling.c',
  'src/benchmarks/vtebench_scrolling_bottom_region.c',
  'src/benchmarks/vtebench_scrolling_bottom_small_region.c',
  'src/benchmarks/vtebench_scrolling_fullscreen.c',
  'src/benchmarks/vtebench_scrolling_top_region.c',
  'src/benchmarks/vtebench_scrolling_top_small_region.c',
  'src/benchmarks/vtebench_sync_medium_cells.c',
  'src/benchmarks/vtebench_unicode.c',
  'src/benchmarks/vtebench_missing.c',
  'vendor/tomlc17/tomlc17.c',
  'vendor/yyjson/src/yyjson.c',
)

subdir('data')

executable(
  'te-stsuite',
  sources,
  include_directories: inc,
  dependencies: [math],
  link_args: ['-static'],
  install: false,
)
