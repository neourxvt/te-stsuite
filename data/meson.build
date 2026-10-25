# Copy runtime workloads beside the executable so its bytes contain no source path.
foreach workload : ['unicode_symbols.txt', 'medium_cells.vim', 'sync_medium_cells.vim']
  configure_file(input: workload, output: workload, copy: true)
endforeach
