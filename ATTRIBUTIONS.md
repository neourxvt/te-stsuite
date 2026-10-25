# Attributions

## Vendored libraries

- **tomlc17** by CK Tan, copyright 2024-2026 CK Tan. The vendored parser is in `vendor/tomlc17/` and is licensed under the MIT License. See the [upstream project](https://github.com/cktan/tomlc17) and its [license](https://github.com/cktan/tomlc17/blob/main/LICENSE).
- **yyjson 0.13.0** by YaoYuan, copyright 2020 YaoYuan. The vendored library is in `vendor/yyjson/` and is licensed under the MIT License; its license text is included at `vendor/yyjson/LICENSE`. See the [upstream project](https://github.com/ibireme/yyjson).

## Reimplemented benchmarks

The C workloads in `src/benchmarks/termbench.c` are ports of **TermBench V2**, copyright (c) 2021 Casey Muratori: [cmuratori/termbench](https://github.com/cmuratori/termbench), based on [revision 074fa615069ae4b53382d5f69143ef15fe1a8dc3](https://github.com/cmuratori/termbench/blob/074fa615069ae4b53382d5f69143ef15fe1a8dc3/termbench.cpp). The upstream project is licensed under the [MIT License](https://github.com/cmuratori/termbench/blob/074fa615069ae4b53382d5f69143ef15fe1a8dc3/LICENSE); its copyright notice and license text are included in [LICENSES/MIT-termbench.txt](LICENSES/MIT-termbench.txt).

- `termbench_many_line` ports `ManyLine`: varied lowercase ASCII with randomly placed newlines, each selected from 27 possible symbols.
- `termbench_long_line` ports `LongLine`: varied lowercase ASCII from 26 possible symbols, with line breaks left to terminal wrapping.
- `termbench_fg_per_char` ports `FGPerChar`: a separate RGB foreground sequence and a letter for each cell.
- `termbench_fgbg_per_char` ports `FGBGPerChar`: separate RGB background and foreground sequences and a letter for each cell.

The text workloads prepare 64 KiB chunks using a local xorshift32 generator seeded with 1 for every preparation. RGB workloads prepare 256 frames, covering a complete byte cycle for each color channel, and paint exactly the terminal's reported rows and columns. The port retains the upstream RGB gradients, background-before-foreground ordering, and a-through-y glyph cycle. The suite repeats prepared payloads to meet `min_bytes` and applies its configured warmup and sampling limits. Payload generation is outside the PTY write timer; timings measure data acceptance and are not directly comparable to upstream's timed RGB generation loop or to displayed frame rates.

The C workloads in `src/benchmarks/vtebench_*.c` and their associated workload data are reimplementations of benchmarks from **vtebench 0.3.1**, by Christian Duerr and Joe Wilm: [alacritty/vtebench](https://github.com/alacritty/vtebench). The upstream project offers them under either the MIT License or Apache License 2.0; see its [MIT license](https://github.com/alacritty/vtebench/blob/master/LICENSE-MIT) and [Apache 2.0 license](https://github.com/alacritty/vtebench/blob/master/LICENSE-APACHE).

Reimplemented workloads: `cursor_motion`, `dense_cells`, `light_cells`, `medium_cells`, `missing` (from `extra_benchmarks`), `scrolling`, `scrolling_bottom_region`, `scrolling_bottom_small_region`, `scrolling_fullscreen`, `scrolling_top_region`, `scrolling_top_small_region`, `sync_medium_cells`, and `unicode`.
