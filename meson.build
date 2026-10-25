project(
  'te-stsuite',
  'c',
  version: '0.1.0',
  meson_version: '>=1.3.0',
  default_options: ['c_std=c17', 'warning_level=3'],
)

# Keep the CLI version tied to the manually maintained Meson project version.
version_config = configuration_data()
version_config.set_quoted('TE_STSUITE_VERSION', meson.project_version())
configure_file(output: 'te-stsuite-version.h', configuration: version_config)

if host_machine.system() != 'linux'
  error('te-stsuite supports modern Linux systems only')
endif

cc = meson.get_compiler('c')
math = cc.find_library('m', required: true)
# Typometer observes rendered X11 pixels and injects real XTEST key events.
x11 = dependency('x11', static: false)
xtst = dependency('xtst', static: false)
threads = dependency('threads')

python = import('python').find_installation('python3', required: true)
embedded_benchmark_data = custom_target(
  'embedded_benchmark_data',
  input: files(
    'data/unicode_symbols.txt',
    'data/medium_cells.vim',
    'data/sync_medium_cells.vim',
  ),
  output: 'embedded_benchmark_data.c',
  command: [
    python,
    files('tools/embed_benchmark_data.py'),
    '@INPUT@',
    '@OUTPUT@',
  ],
)

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

inc = include_directories('.', 'include', 'vendor/tomlc17', 'vendor/yyjson/src')
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
  'src/monitor.c',
  'src/monitors/polling.c',
  'src/monitors/cgroupv2.c',
  'src/process_isolations/taskset.c',
  'src/process_isolations/cgroupv2.c',
  'src/worker.c',
  'src/process_isolation.c',
  'src/controller.c',
  'src/benchmarks.c',
  'src/benchmarks/startup.c',
  'src/benchmarks/typometer.c',
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

sources += embedded_benchmark_data

executable(
  'te-stsuite',
  sources,
  include_directories: inc,
  dependencies: [math, x11, xtst, threads],
  install: false,
)
