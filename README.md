# te-stsuite

`te-stsuite` benchmarks terminal emulator startup and PTY rendering workloads on
modern Linux. The workloads are implemented in C and are based on the built-in
benchmarks from `vtebench`, including its missing-glyph workload.

## Build and run

The build requires Meson 1.3 or newer, Python 3, `musl-gcc`, and Linux procfs.
The checked-in Meson native file selects musl-gcc, and `meson.build` links the executable
statically. It also maps checkout and build paths out of the binary, rejects
compile-time date/time macros, and embeds benchmark payloads in the executable.
Executable binaries built from the same source and toolchain are byte-identical
across checkout and build paths.
Build with the convenience script:

```sh
./build.sh
```

To configure Meson directly, pass the native file on the initial setup:

```sh
meson setup build --native-file meson/musl.ini
meson compile -C build
```

Meson chooses its compiler during setup, so direct builds need this native
file too. To use a different musl compiler, update the `c` entry in
`meson/musl.ini`.

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

[kitty]
cmd = "kitty --config {CFG} {CMD}"

[kitty.configs]
compact = """
font_size 11.0
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

Use repeatable `-t`/`--terminal` and `-b`/`--benchmark` selectors to limit a
run. Terminal names match the named TOML tables; selecting a terminal with
config variants runs every variant, while a full name such as `kitty/compact`
selects just that variant. Benchmark names are listed by `--help`.

```sh
./build/te-stsuite example.toml --terminal kitty --terminal xterm
./build/te-stsuite example.toml --benchmark startup --benchmark vtebench_unicode
./build/te-stsuite example.toml --terminal kitty --benchmark vtebench_scrolling
```

Each configured terminal launches sequentially. The `{CMD}` placeholder is
expanded to the current executable, the TOML path, `--run`, and a private local
worker control pipe option. A terminal can define multiple inline configuration
variants under `[kitty.configs]`, with each key naming a variant and its
multiline string supplying the content. Each variant is run separately; its
content is written to a temporary file and the standalone `{CFG}` command word
is replaced with that file's path. Commands without inline variants must not
contain `{CFG}`. The worker runs inside the terminal's PTY. A terminal can
define a nested `xresources` table, for example `[xterm.xresources]`, with each
key naming an X resource and each single-line string, integer, float, or
boolean supplying its value. The table is serialized to a temporary file for
each terminal launch and exposed through `XENVIRONMENT`; the file is removed
when the launch ends. Config variants inherit the terminal's X resources. An
empty table supplies an empty per-run file, and leaving the table out preserves
the inherited environment. This adds resources for X clients that honor
`XENVIRONMENT` while retaining resources already loaded into the display.
Terminal command strings use whitespace, single or double quotes, and
backslash escapes; they are split into arguments without shell expansion.

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

Before measuring, te-stsuite verifies the selected process isolation mode. When
cgroups are in use, it also checks that each terminal and its worker are placed
in separate accounting cgroups. It then launches every selected terminal once
for a separate warmup phase. It uses the same configured command for checks,
warmup, and measured runs. In
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
once during the first warmup; output from process isolation checks, later
warmups, and startup samples is suppressed.
Each later result measures PTY writes after the
separate warmup phase. The printed workload wall time covers all measured
writes; the write average, 90th percentile, and standard deviation cover
payload writes and exclude the per-sample reset and setup bytes.

The report is printed after every benchmark and as a collective summary after
all terminals finish. Matching logs are created in the current working
directory as `.log.te-stsuite-YYYYMMDD-HHMMSS.log` and
`.log.te-stsuite-YYYYMMDD-HHMMSS.jsonl`.

The JSONL file is compact and contains run metadata, one process isolation
check record per terminal, at most one warmup output record, one flat result
record per measured terminal and benchmark, and one end record. The warmup
output record includes captured stdout and stderr from the first warmup.
Output from process isolation checks, later warmups, and startup samples is
suppressed and is not recorded.
Results are not duplicated for printed summaries. The schema version is 5.
Durations use seconds or milliseconds as named, memory uses
bytes, and `cpu_pct` is percent of one core. Missing resource measurements are
JSON null. Workload results also include payload size and write-time statistics;
partial results are marked with `"complete":false`. A check record's
`cgroups_used` field indicates whether cgroup scopes were used, and
`worker_moved` indicates whether the worker joined its separate cgroup.

```jsonl
{"type":"run","schema":5,"started_at":"2026-10-25T08:00:00Z","config":"/path/to/example.toml","monitor":"cgroupv2","process_isolation":"cgroupv2"}
{"type":"process_isolation_check","terminal":"kitty/compact","status":"passed","cgroups_used":true,"worker_moved":true,"detail":"terminal isolated on CPU 2; worker is in its separate cgroup"}
{"type":"warmup_output","terminal":"kitty/compact","stdout":"","stderr":""}
{"type":"result","terminal":"kitty/compact","benchmark":"startup","wall_s":0.08,"resources_available":true,"samples":10,"payload_bytes":0,"complete":true,"cpu_s":0.01,"cpu_pct":12.5,"mem_avg_bytes":7340032,"mem_peak_bytes":8388608,"startup_avg_ms":80,"startup_p90_ms":84,"startup_sd_ms":3}
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
Unicode symbols, and missing glyphs.
