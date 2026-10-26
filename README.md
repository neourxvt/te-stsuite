# te-stsuite

`te-stsuite` benchmarks terminal emulator startup, typing latency, and PTY rendering workloads on
modern Linux. The workloads are implemented in C and are based on TermBench V2
and the benchmarks from `vtebench`, including its missing-glyph workload.

## Build and run

The build requires Meson 1.3 or newer, Ninja, Python 3, a C17 compiler,
libX11 and libXtst development packages, POSIX threads, and Linux procfs.
Meson uses the system C compiler and its default libc, such as glibc on glibc-based
distributions or musl on Alpine, with dynamic linking by default.
The build maps checkout and build paths out of the binary, rejects
compile-time date/time macros, and embeds benchmark payloads in the executable.
Executable binaries built from the same source and toolchain are byte-identical
across checkout and build paths.
Build with the convenience script:

```sh
./build.sh
```

To configure Meson directly:

```sh
meson setup build
meson compile -C build
```

To select a different compiler, set `CC` when first configuring the build
directory, for example `CC=clang ./build.sh`. Meson caches the compiler choice;
use a fresh build directory when switching compilers, including when moving
from an existing musl-gcc build to the system compiler.

Start with [example.toml](example.toml), or create a TOML file with one named
table for each terminal. Each `cmd` must contain one standalone `{CMD}` word
where the te-stsuite worker will be inserted:

```toml
min_bytes = 1048576
warmup = 1
max_seconds = 10
max_samples = 0
startup_min_seconds = 3
startup_min_samples = 10
startup_timeout_seconds = 30
monitor = "cgroupv2"
process_isolation = "taskset"

[variables]
BASE00 = "#181818"
BASE05 = "#d8d8d8"

[kitty]
cmd = "kitty --config {CFG} {CMD}"

[kitty.configs]
compact = """
font_size 11.0
background {BASE00}
foreground {BASE05}
"""

large = """
font_size 14.0
"""

[foot]
cmd = "foot {CMD}"

[xterm]
cmd = "xterm -e {CMD}"

[xterm.xresources]
"xterm*faceName" = "monospace"
"xterm*faceSize" = 11
```

Run the benchmark suite by default, or validate the configuration without
launching any terminals:

```sh
./build/te-stsuite terminals.toml
./build/te-stsuite terminals.toml --validate
./cycle.sh example.toml
```

When the user systemd manager does not delegate `cpuset`, run through the
separate system-manager launcher with an explicit allocation of at least two
CPUs:

```sh
./cycle-sudo.sh --cpus 2,4 example.toml -b startup
```

Replace `2,4` with the CPUs to reserve. The launcher builds as the current user
and runs the suite as that user in a transient cpuset partition. It requires
systemd 261 or newer and sudo permission to start system services. The CPUs are
released when the service exits. `cycle.sh` is unchanged.

Use `-t`/`--terminal` and `-b`/`--benchmark` to list names or select entries
for a run. Bare `-b` prints help and all benchmarks without needing a config
file. Bare `-t` prints help without a config; pass a config to also list its
configured terminals. Add a name to either option to select it for the run;
repeat named selectors to select more. Terminal names match the named TOML
tables; selecting a terminal with config variants runs every variant, while a
full name such as `kitty/compact` selects just that variant. The benchmark name
`typometer` selects every named Typometer configuration; `typometer/basic`
selects only `basic`. Passing a config to bare `-b` also lists its Typometer
configuration names.

```sh
./build/te-stsuite -b
./build/te-stsuite example.toml -t
./build/te-stsuite example.toml --terminal kitty --terminal xterm
./build/te-stsuite example.toml --benchmark startup --benchmark vtebench_unicode
./build/te-stsuite example.toml --terminal kitty --benchmark vtebench_scrolling
```

Each configured terminal launches sequentially. The `{CMD}` placeholder is
expanded to the current executable, the TOML path, `--run`, and a private local
worker control pipe option. A terminal can define multiple inline configuration
variants under `[kitty.configs]`, with each key naming a variant and its
multiline string supplying the content. Each variant is run separately; its
content is written to `config.toml` in a private temporary directory and the
`{CFG}` command reference is replaced with that file's path, either as a whole
argument or embedded in one, such as `--config-file={CFG}`. A terminal can
instead pass the path through its `env` table. Values equal to `{CFG}` or
`{CFG_DIR}` receive the config file or directory path; other strings are passed
literally. For example, Rio can use `cmd = "rio -e {CMD}"` with
`RIO_CONFIG_HOME = "{CFG_DIR}"` under `[rio.env]`. Config references require
inline variants, and each variant must reference its config through the command
or environment. Environment overrides apply only to that terminal launch and
are inherited by all its variants. The temporary file and directory are removed
when the launch ends. Commands without inline variants must not contain `{CFG}`.
The worker runs inside the terminal's PTY. A terminal can
define a nested `xresources` table, for example `[xterm.xresources]`, with each
key naming an X resource and each single-line string, integer, float, or
boolean supplying its value. The table is serialized to a temporary file for
each terminal launch and exposed through `XENVIRONMENT`; the file is removed
when the launch ends. Config variants inherit the terminal's X resources. An
empty table supplies an empty per-run file, and leaving the table out preserves
the inherited environment. This adds resources for X clients that honor
`XENVIRONMENT` while retaining resources already loaded into the display. A
top-level `[variables]` table defines string substitutions such as
`BASE00 = "#181818"`; matching `{NAME}` references are replaced in TOML string
values, including commands, inline configs, environment values, and X resource
values. Unmatched placeholders such as `{CMD}` and `{CFG}` are kept for the
terminal launcher. See the Base16 Default Dark values and terminal palette
mappings in [example.toml](example.toml). Terminal command strings use
whitespace, single or double quotes, and backslash escapes; they are split into
arguments without shell expansion.
Each terminal table can also include an optional one-line `version` entry such
as `version = { cmd = "urxvt -h", head = 2 }`. The Precheck stage runs the
command and prints its combined output; `head` limits it to the first `N` lines.
Set `silence_output = true` in a terminal table to suppress its stdout and
stderr during launches. Configured version output remains visible.

The `process_isolation` setting selects `cgroupv2` or `taskset` and defaults to
`taskset`. In `cgroupv2` mode, each terminal runs in an isolated cpuset
partition on one automatically selected exclusive CPU, while the benchmark
worker runs on the remaining CPUs. This requires the cpuset controller, a
writable delegated cgroup v2 subtree whose current cgroup is a valid partition
root, and at least two effective CPUs. The partition removes the selected
logical CPU from scheduler load balancing and unbound workqueues; it does not
move interrupts or isolate SMT sibling threads. In `taskset` mode, the terminal
and worker are pinned to CPU 0 using scheduler affinity. CPU 0 must be available
to the process, and this mode does not reserve that CPU from other workloads.
With `monitor = "polling"`, taskset mode runs without cgroups. The cgroupv2
monitor still requires cgroups for resource accounting. Avoid terminal
commands that attach to an already-running daemon, since the new process must
own the measured PTY session.

To run without cgroups, configure `monitor = "polling"`; process isolation
defaults to `taskset`.

## Measurements

The Precheck stage prints each configured terminal version and verifies the
selected process isolation mode. When cgroups are in use, it also checks that
each terminal and its worker are placed in separate accounting cgroups. It
then launches every selected terminal once for a separate warmup phase. The
configured terminal command is used for isolation checks, warmup, and measured
runs. In
`cgroupv2` mode, the worker runs on the remaining CPUs outside the isolated
partition. Its resource use does not count toward the terminal.

The `monitor` setting independently selects `cgroupv2` or `polling` and defaults
to `cgroupv2`. The `cgroupv2` backend reads terminal CPU time from `cpu.stat`,
and memory from `memory.current` and `memory.peak`; memory includes charged cache
and other cgroup memory, not only process RSS. The `polling` backend uses the
`/proc` process-tree sampler and reports summed RSS. Both backends report CPU as
a percentage of one core and memory as time-weighted average and peak MiB. Each
startup and workload result has its own interval. The cgroupv2 monitor and
cgroupv2 process isolation require a writable delegated cgroup v2 subtree;
cgroupv2 process isolation additionally requires cpuset partition support.
Missing resource counters are recorded as null; te-stsuite does not turn
missing counter values into zero or silently switch backends.

The startup benchmark launches the configured terminal repeatedly and measures
from each launch until the in-terminal worker is ready. It gathers at least ten
samples and at least three seconds of measured startup time by default. The
startup result reports the mean wall time, 90th percentile, and standard
deviation across those launches. `max_samples` can cap the startup sample count
as well as workload samples. Terminal stdout and stderr are shown and recorded
once per terminal during its warmup unless `silence_output = true`; output from
process isolation checks and startup samples is suppressed.
Each later result measures PTY writes after the
separate warmup phase. The printed workload wall time covers all measured
writes; the write average, 90th percentile, and standard deviation cover
payload writes and exclude the per-sample reset and setup bytes.

The report is printed after every benchmark and as a collective summary after
all terminals finish. Matching logs are created in the current working
directory as `.log.te-stsuite-YYYYMMDD-HHMMSS.log` and
`.log.te-stsuite-YYYYMMDD-HHMMSS.jsonl`.

The JSONL file is compact and contains run metadata, one process isolation
check record per terminal, at most one warmup output record per terminal, one
flat result record per measured terminal and benchmark, and one end record.
Each warmup output record includes that terminal's captured stdout and stderr,
or empty strings when `silence_output = true`.
Precheck version output is printed and written to the text log. Process
isolation check output and startup samples are suppressed and are not recorded.
Results are not duplicated for printed summaries. The schema version is 8.
Durations use seconds or milliseconds as named, memory uses
bytes, and `cpu_pct` is percent of one core. Missing resource measurements are
JSON null. Workload results also include payload size and write-time statistics;
partial results are marked with `"complete":false`. A check record's
`cgroups_used` field indicates whether cgroup scopes were used, and
`worker_moved` indicates whether the worker joined its separate cgroup.
Each result's `resources_collected` field records whether its benchmark used
interval collection. A benchmark can opt out with its
`Benchmark.skip_resource_collection` flag.
When false, the wall, CPU, and memory fields are null,
`resources_available` is null, and that result is omitted from the collective
resource table. Process isolation checks still run for these benchmarks.
The `cell_dimensions` result also records `cell_width_px` and
`cell_height_px`.

```jsonl
{"type":"run","schema":8,"started_at":"2026-10-25T08:00:00Z","config":"/path/to/example.toml","monitor":"cgroupv2","process_isolation":"cgroupv2"}
{"type":"process_isolation_check","terminal":"kitty/compact","status":"passed","cgroups_used":true,"worker_moved":true,"detail":"terminal isolated on CPU 2; worker is in its separate cgroup"}
{"type":"warmup_output","terminal":"kitty/compact","stdout":"","stderr":""}
{"type":"result","terminal":"kitty/compact","benchmark":"startup","resources_collected":true,"wall_s":0.08,"resources_available":true,"samples":10,"payload_bytes":0,"complete":true,"cpu_s":0.01,"cpu_pct":12.5,"mem_avg_bytes":7340032,"mem_peak_bytes":8388608,"startup_avg_ms":80,"startup_p90_ms":84,"startup_sd_ms":3}
{"type":"end","status":"ok","results":1}
```

Configuration options default to 1 MiB minimum payloads, one warmup pass per
workload, ten seconds per measured workload, unlimited samples within that
time, ten startup samples over at least three seconds, and a 30 second timeout
for each terminal launch. The startup `wall_s` and `startup_avg_ms` fields are
the mean of the startup samples; `startup_p90_ms` and `startup_sd_ms` report
their 90th percentile and sample standard deviation. `max_samples` can cap
sample counts; it does not extend `max_seconds`.

## Workloads

The suite runs startup first, followed by the ported `vtebench_*` workloads:
cursor motion, dense cells, light cells, medium cells, scrolling, four
scrolling-region workloads, scrolling fullscreen, synchronized medium cells,
Unicode symbols, and missing glyphs. The `termbench_many_line` and
`termbench_long_line` workloads then stream varied lowercase ASCII with and
without explicit newlines. The `termbench_fg_per_char` and
`termbench_fgbg_per_char` workloads repaint the terminal grid with per-cell RGB
foreground colors or RGB foreground and background colors. See
[ATTRIBUTIONS.md](ATTRIBUTIONS.md) for their source and adaptation details.
The `typometer` typing latency benchmark runs after the PTY workloads, followed
by the `cell_dimensions` rendered-cell check.

## Typing latency

The `typometer` benchmark ports the measurement backend from Typometer C++ to C.
It requires a native X11 session with XTEST and a visible terminal using a
monospaced font and a uniform, opaque background. Native Wayland and XWayland
measurement are unsupported. The benchmark finds the terminal window through
`_NET_WM_PID` and its worker's process ancestry, and focuses it automatically.
Do not type, hold modifiers, move or obscure the window, or change focus during
a run. Launch a separate terminal instance rather than attaching to a daemon.

```sh
./build/te-stsuite example.toml -t xterm -b typometer
```

The worker provides a minimal raw-mode typing target inside the terminal;
no editor is needed. Five injected periods calibrate character spacing,
sampling pixels, the background, and block cursor handling. Each sample times
XTEST key injection until the corresponding pixel changes on the X11 screen.
This includes input dispatch, the worker's echo, PTY transport, and visible
rendering through X11. It excludes physical keyboard hardware and monitor
scanout. Calibration uses screen reads internally; it saves no images.

Define any number of Typometer configurations in named TOML tables:

```toml
[typometer.basic]
count = 100
delay_ms = 0
asynchronous = false
pause_every = 0
pause_ms = 1000

[typometer.paced]
count = 100
delay_ms = 10
asynchronous = true
pause_every = 20
pause_ms = 1000
```

Each table independently defaults to `count = 100`, `delay_ms = 0`,
`asynchronous = false`, `pause_every = 0` (disabled), and `pause_ms = 1000`.
`count` must be positive; timing and pause values must be nonnegative integers,
and `asynchronous` must be a boolean. Names must be nonempty, at most 117 bytes,
and contain no spaces, control characters, or slashes. The root `typometer_*`
options have been replaced by these table entries. When no Typometer tables
are defined, the suite runs one `typometer` workload with the defaults.

`-b typometer` and runs without a benchmark selector execute every configuration
in file order, warming and reporting each separately as `typometer/basic`,
`typometer/paced`, and so on. Selecting both the group and an individual
configuration runs each matching configuration once:

```sh
./build/te-stsuite example.toml -b typometer
./build/te-stsuite example.toml -b typometer/basic
./build/te-stsuite example.toml -b
```

Periodic pauses replace the ordinary delay. Synchronous mode waits for each screen change
before applying the delay; asynchronous mode uses a separate input thread
that can queue characters ahead of rendering. Both modes delete completed
batches with Backspace. `max_samples` can cap the character count, and
`max_seconds` bounds typing and delays. An in-flight sample can take up to
three further seconds before timing out; batch cleanup also needs time to
settle. Each warmup pass runs the same workload without recording results.

Reports include mean, 90th percentile, and sample standard deviation of typing
latency for each configuration. JSONL results identify it in `benchmark` and
use `latency_avg_ms`, `latency_p90_ms`, and
`latency_sd_ms`, with `samples` counting characters and `payload_bytes` zero.
CPU and memory use the selected te-stsuite monitoring backend. Their interval
excludes calibration and includes typing, delays, pauses, and cleanup. Focus
changes, unexpected input, and missing pixel updates fail the run; terminal
input settings are restored on completion, failure, and handled termination
signals. A failed run does not report successful latency samples; its partial result
uses null latency fields.

The ported Typometer backend is licensed under Apache-2.0. See
[NOTICE.typometer](NOTICE.typometer) and
[LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) for attribution and terms.

## Cell dimensions

The `cell_dimensions` benchmark writes one space with a truecolor red
background, resets the background color, and captures the visible terminal
window in memory. It measures the bounding box of reddish pixels and reports
the cell width and height in pixels in the benchmark output and JSONL result.
It does not save an image file or collect CPU, memory, or wall-time metrics.
The benchmark requires a native X11 session with a visible, fully on-screen
terminal window; it does not require XTEST.

```sh
./build/te-stsuite example.toml -t xterm -b cell_dimensions
```
