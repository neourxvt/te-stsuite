# Attributions

## Vendored libraries

- **tomlc17** by CK Tan, copyright 2024-2026 CK Tan. The vendored parser is in `vendor/tomlc17/` and is licensed under the MIT License. See the [upstream project](https://github.com/cktan/tomlc17) and its [license](https://github.com/cktan/tomlc17/blob/main/LICENSE).
- **yyjson 0.13.0** by YaoYuan, copyright 2020 YaoYuan. The vendored library is in `vendor/yyjson/` and is licensed under the MIT License; its license text is included at `vendor/yyjson/LICENSE`. See the [upstream project](https://github.com/ibireme/yyjson).

## Reimplemented benchmarks

The C workloads in `src/benchmarks/vtebench_*.c` and their associated workload data are reimplementations of benchmarks from **vtebench 0.3.1**, by Christian Duerr and Joe Wilm: [alacritty/vtebench](https://github.com/alacritty/vtebench). The upstream project offers them under either the MIT License or Apache License 2.0; see its [MIT license](https://github.com/alacritty/vtebench/blob/master/LICENSE-MIT) and [Apache 2.0 license](https://github.com/alacritty/vtebench/blob/master/LICENSE-APACHE).

Reimplemented workloads: `cursor_motion`, `dense_cells`, `light_cells`, `medium_cells`, `missing` (from `extra_benchmarks`), `scrolling`, `scrolling_bottom_region`, `scrolling_bottom_small_region`, `scrolling_fullscreen`, `scrolling_top_region`, `scrolling_top_small_region`, `sync_medium_cells`, and `unicode`.
