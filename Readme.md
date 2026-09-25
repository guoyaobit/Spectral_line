# Spectral_line

High-throughput DPDK/CUDA receiver for 7 mm spectral-line observations. It
receives VDIF-over-UDP input, performs PFB/FFT and Stokes accumulation on GPUs,
and either sends complete spectrum results over ZeroMQ/TCP or records VDIF
baseband files.

See [docs/deployment.md](docs/deployment.md) for host preparation, build,
configuration, and start-up checks.

Each of the two 100G DPDK interfaces receives UDP destination ports
`60000–60007`. Ports `60000/60001` carry X/Y polarisation for one subband,
`60002/60003` carry the next subband, and so on through `60006/60007`. DPDK
port 0 supplies subbands 0–3 and DPDK port 1 supplies subbands 4–7. These input
ports are independent of the ZeroMQ/TCP result destination ports configured
under `subbands[].port`. In the example configuration those output destination
ports also use `60000–60007`, but they belong to the separate kernel-managed
SR-IOV result path. Result ports are freely configurable, may be non-contiguous,
and may be shared by multiple subbands. The Writer identifies the subband,
beam, and window from the message header rather than the TCP port. Each message
contains one complete window result; if the
Writer is offline or the bounded ZeroMQ queue is full, that complete result is
dropped without blocking GPU processing.

To distribute the controller's source tree, compile it on multiple servers,
enable the monitor service, and install the receiver service for coordinated
manual startup, use the included [Ansible playbook](ansible/README.md).

To inspect an 8-bit VDIF output file with DiFX tools:

```sh
m5test <file> VDIF_8192_62500m1-8-1
```

To validate the FPGA's live VDIF-over-UDP headers, packet length, 62500-frame
cadence, and packet continuity without starting the DPDK receiver:

```sh
tests/check_vdif_udp.py 10.17.16.11 60002
```

Monitor all eight ports on the same IP concurrently (the packet count applies
to each port independently). The checker uses one process and one receive
socket per port so parsing can run on separate CPU cores:

```sh
tests/check_vdif_udp.py 10.17.16.11 60000-60007
```

The default test receives 125000 packets (about two seconds) per port and
stops a quiet port after five seconds. Override these only when needed, for
example `-n 625000 -t 10`.

At full FPGA line rate, Python and the kernel UDP stack may still become the
bottleneck and report receiver-side loss. Use this tool for header/cadence
checks; use NIC/DPDK counters for authoritative full-rate packet-loss tests.

The checker reports the first packet header, UTC second boundaries, missing,
duplicate, or reordered frames, invalid flags, and a final summary. Use
`--expect-version`, `--expect-edv`, or `--expect-thread-id` when those FPGA
fields must have a specific value. Run its offline parser check with
`python3 tests/check_vdif_udp.py --self-test`.

Install the monitor dependencies and start the continuous image generator:

```sh
python -m pip install -r monitor/requirements.txt
python monitor/plot_monitor.py
```

With no arguments, the script continuously scans
`/dev/shm/server_*_stream_*.bin`. Changed streams are rendered at most once
every two seconds. Worker processes regenerate three 180-DPI images and three
320x120 thumbnails directly under `/dev/shm/monitor_plots/`; no
quality-specific subdirectories are created. Thumbnails are downscaled from
the high-quality render instead of being plotted a second time. Images are replaced atomically,
so readers never observe partially written JPEGs. Image names use
`<subband>_<beam>_<polarization>_<type>_<quality>.jpg`, where subband is
`1–32`, beam is `A` or `B`, polarization is `X` or `Y`, type is
`1` (raw ADC), `2` (normal-distribution histogram), or `3` (frequency
spectrum), and quality is `hq` or `lq`. For example:
`1_A_X_1_hq.jpg` and `2_B_Y_2_lq.jpg`. Run
`python monitor/plot_monitor.py --once` for a single update.
After the first successful JPEG manifest is published, legacy
`server_*_stream_*.png` images and images in the old `high_resolution` and
`thumbnails` directories are removed.

The receiver creates a stream's monitor file only after that stream receives
its first valid packet. Missing-input streams therefore do not appear as empty
or zero-filled monitor files.

The monitor supports all 16 streams updating in the same cycle. By default it
uses up to four rendering processes, capped by the host's logical CPU count.
Set `MONITOR_PLOT_WORKERS` and `MONITOR_PLOT_INTERVAL_SECONDS` in
`/etc/default/spectral-line-monitor` to tune concurrency and refresh rate when
CPU or memory must be reserved for the DPDK receiver. See `docs/deployment.md`
for the systemd installation steps.

When every changed stream in a scan has rendered successfully, the service
atomically replaces `/dev/shm/monitor_plots/manifest.json`. A failed render
leaves the previous manifest unchanged and is retried on the next scan.
