# cvtebench

`cvtebench` benchmarks terminal emulator startup and PTY rendering workloads on
modern Linux. The workloads are implemented in C and are based on the built-in
benchmarks from `vtebench`, including its missing-glyph workload.

## Build and run

The build requires Meson 1.3 or newer, `musl-gcc`, and Linux procfs. The checked-in Meson
native file selects musl-gcc, and `meson.build` links the executable
statically. It also maps checkout and build paths out of the binary, rejects
compile-time date/time macros, and places runtime data beside the executable.
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
where the cvtebench worker will be inserted:

```toml
min_bytes = 1048576
warmup = 1
max_seconds = 10
max_samples = 0
startup_timeout_seconds = 30
monitor = "cgroupv2"

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
```

Run the benchmark suite by default, or validate the configuration without
launching any terminals:

```sh
./build/cvtebench terminals.toml
./build/cvtebench terminals.toml --validate
./cycle.sh example.toml
```

Use repeatable `-t`/`--terminal` and `-b`/`--benchmark` selectors to limit a
run. Terminal names match the named TOML tables; selecting a terminal with
config variants runs every variant, while a full name such as `kitty/compact`
selects just that variant. Benchmark names are listed by `--help`.

```sh
./build/cvtebench example.toml --terminal kitty --terminal xterm
./build/cvtebench example.toml --benchmark startup --benchmark unicode
./build/cvtebench example.toml --terminal kitty --benchmark scrolling
```

Each configured terminal launches sequentially. The `{CMD}` placeholder is
expanded to the current executable, the TOML path, `--run`, and a private local
worker socket option. A terminal can define multiple inline configuration
variants under `[kitty.configs]`, with each key naming a variant and its
multiline string supplying the content. Each variant is run separately; its
content is written to a temporary file and the standalone `{CFG}` command word
is replaced with that file's path. Commands without inline variants must not
contain `{CFG}`. The worker runs inside the terminal's PTY. Terminal command
strings use whitespace, single or double quotes, and backslash escapes; they
are split into arguments without shell expansion.

The terminal process starts with CPU affinity restricted to CPU 0. If CPU 0 is
unavailable to the current Linux cpuset, that terminal is skipped with an
error. Avoid terminal commands that attach to an already-running daemon, since
the new process must own the measured PTY session.

## Measurements

Before measuring, cvtebench checks whether each terminal and its worker are
placed in separate cgroups, then launches every selected terminal once for a
separate warmup phase. It uses the same configured command for checks, warmup,
and measured runs. The worker is placed in a sibling cgroup so its resource use
does not count toward the terminal.

The `monitor` setting selects `cgroupv2` or `polling` and defaults to
`cgroupv2`. Cgroup v2 reads terminal CPU time from `cpu.stat`, and memory from
`memory.current` and `memory.peak`; memory includes charged cache and other
cgroup memory, not only process RSS. The `polling` backend retains the prior
`/proc` process-tree sampler and reports summed RSS. Both backends report CPU as
a percentage of one core and memory as time-weighted average and peak MiB. Each
startup and workload result has its own interval. If cgroup delegation is not
available, the check says so and cgroup resource values are recorded as null;
cvtebench does not turn missing counters into zero or silently switch backends.

The startup result measures time from launching the configured command until
the in-terminal worker is ready. Each later result measures PTY writes after
the separate warmup phase. The printed wall time covers the measured workload;
the write average, 90th percentile, and standard deviation cover payload writes
and exclude the per-sample reset and setup bytes.

The report is printed after every benchmark and as a collective summary after
all terminals finish. Matching logs are created in the current working
directory as `cvtebench-YYYYMMDD-HHMMSS.log` and
`cvtebench-YYYYMMDD-HHMMSS.jsonl`.

The JSONL file is compact and contains run metadata, one cgroup check record per
terminal, one flat result record per measured terminal and benchmark, and one
end record. Results are not duplicated for printed summaries. The schema
version is 2. Durations use seconds or milliseconds as named, memory uses
bytes, and `cpu_pct` is percent of one core. Missing resource measurements are
JSON null. Workload results also include payload size and write-time statistics;
partial results are marked with `"complete":false`.

```jsonl
{"type":"run","schema":2,"started_at":"2026-10-25T08:00:00Z","config":"/path/to/example.toml","monitor":"cgroupv2"}
{"type":"cgroup_check","terminal":"kitty/compact","status":"inside","worker_moved":true,"detail":"terminal and worker are in separate cgroups"}
{"type":"result","terminal":"kitty/compact","benchmark":"startup","wall_s":0.08,"resources_available":true,"samples":1,"payload_bytes":0,"complete":true,"cpu_s":0.01,"cpu_pct":12.5,"mem_avg_bytes":7340032,"mem_peak_bytes":8388608}
{"type":"end","status":"ok","results":1}
```

Configuration options default to 1 MiB minimum payloads, one warmup pass per
workload, ten seconds per measured workload, unlimited samples within that
time, and a 30 second startup timeout. `max_samples` can cap the sample count;
it does not extend `max_seconds`.

## Workloads

The suite runs startup first, followed by cursor motion, dense cells, light
cells, medium cells, scrolling, four scrolling-region workloads, scrolling
fullscreen, synchronized medium cells, Unicode symbols, and missing glyphs.
