# Spectral_line

High-throughput DPDK/CUDA receiver for 7 mm spectral-line observations. It
receives VDIF-over-UDP input, performs PFB/FFT and Stokes accumulation on GPUs,
and either sends spectrum windows over UDP or records VDIF baseband files.

See [docs/deployment.md](docs/deployment.md) for host preparation, build,
configuration, and start-up checks.

To inspect an 8-bit VDIF output file with DiFX tools:

```sh
m5test <file> VDIF_8192_62500m1-8-1
```

Install the monitor dependencies and start the continuous image generator:

```sh
python -m pip install -r requirements-monitor.txt
python plot_monitor.py
```

With no arguments, the script continuously scans
`/dev/shm/server_*_stream_*.bin`. Whenever a stream changes, worker processes
simultaneously regenerate its three 300-DPI images and three 320x120 thumbnails
under `/dev/shm/monitor_plots/`. Thumbnails are downscaled from the high-quality
render instead of being plotted a second time. Images are replaced atomically,
so readers never observe partially written PNGs. Run `python plot_monitor.py
--once` for a single update.

The monitor supports all 16 streams updating in the same cycle. By default it
uses up to 16 rendering processes, capped by the host's logical CPU count. Set
`MONITOR_PLOT_WORKERS` in `/etc/default/spectral-line-monitor` to reduce
concurrency when CPU or memory must be reserved for the DPDK receiver. See
`docs/deployment.md` for the systemd installation steps.

When every changed stream in a scan has rendered successfully, the service
atomically replaces `/dev/shm/monitor_plots/manifest.json`. A failed render
leaves the previous manifest unchanged and is retried on the next scan.
