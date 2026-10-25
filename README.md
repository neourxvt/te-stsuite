# cvtebench

`cvtebench` benchmarks terminal emulator startup and PTY rendering workloads on
modern Linux. The workloads are implemented in C and are based on the built-in
benchmarks from `vtebench`, including its missing-glyph workload.

## Build and run

The build requires Meson, `musl-gcc` (or another musl-targeting compiler), and
Linux procfs. `build.sh` uses musl and links the executable statically by
default:

```sh
./build.sh
```

Start with [example.toml](example.toml), or create a TOML file with one named
table for each terminal. Each `cmd` must contain one standalone `{CMD}` word
where the cvtebench worker will be inserted:

```toml
min_bytes = 1048576
warmup = 1
max_seconds = 10
max_samples = 0
startup_timeout_seconds = 30

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

The startup result measures time from launching the configured command until
the in-terminal worker is ready. Each later result covers its own warmups and
PTY writes. CPU time and resident memory are sampled from the terminal process
tree, excluding the benchmark worker: CPU is shown as a percentage of one core,
and memory is shown as time-weighted average and peak RSS in MiB. Each terminal
and benchmark receives its own interval.
The printed wall time covers the full workload interval, including warmups;
the write average, 90th percentile, and standard deviation cover measured
payload writes and exclude the per-sample reset and setup bytes.

The report is printed after every benchmark and as a collective summary after
all terminals finish. A matching log is created in the current working
directory as `cvtebench-YYYYMMDD-HHMMSS.log`.

Configuration options default to 1 MiB minimum payloads, one warmup, ten
seconds per workload, unlimited samples within that time, and a 30 second
startup timeout. `max_samples` can cap the sample count; it does not extend
`max_seconds`.

## Workloads

The suite runs startup first, followed by cursor motion, dense cells, light
cells, medium cells, scrolling, four scrolling-region workloads, scrolling
fullscreen, synchronized medium cells, Unicode symbols, and missing glyphs.
