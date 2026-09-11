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

Generate ADC, FFT, and histogram images for every monitor stream published in
`/dev/shm`:

```sh
python -m pip install -r requirements-monitor.txt
python plot_monitor.py
```

The script scans `/dev/shm/server_*_stream_*.bin` and writes three 300-DPI
images per stream to `/dev/shm/monitor_plots/high_resolution/`, plus three
smaller previews per stream to `/dev/shm/monitor_plots/thumbnails/`. Pass a
different directory as the first argument, use `--output-dir` to select another
output directory, or use `--samples` to control the ADC samples displayed.
