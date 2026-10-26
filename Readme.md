# Spectral_line

High-throughput DPDK/CUDA receiver for 7 mm spectral-line observations. It
receives VDIF-over-UDP input, performs PFB/FFT and Stokes accumulation on GPUs,
and either sends complete spectrum results over ZeroMQ/TCP or records VDIF
baseband files.

See [docs/deployment.md](docs/deployment.md) for host preparation, build,
configuration, and start-up checks.

See [the server/subband allocation workbook](docs/数据分配新.xlsx) for the
supplied server-to-subband reference. This document is not loaded by the
program; runtime settings remain in `config.yaml` and `ansible/configs/GPU*.yaml`.

See [docs/result-data-format.md](docs/result-data-format.md) for the exact
ZeroMQ result-message layout, header fields, CRC32C coverage, spectrum and
continuum payload definitions, frequency coordinates, and receiver validation
rules.

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

The deployed FPGA's boundary value `frame=62500` is accepted as an alias for
`frame=0` of the following second. It is normalized for UTC and boundary
reporting; values greater than 62500 remain header errors.

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

## Plot a baseband VDIF spectrum

The baseband recorder writes one complex VDIF stream per polarization using
8-, 4-, or 2-bit two's-complement I/Q components. Plot an averaged spectrum
from one file (or consecutive rotated parts) with:

```sh
python scripts/plot_vdif_spectrum.py \
  7mm_sb00_A_X_part0000.vdif \
  --center-frequency-hz 512e6 \
  --csv sb00_A_X_spectrum.csv
```

The tool reads the sample width and frame length from each VDIF header, checks
frame continuity, skips frames marked invalid, and reports missing, duplicate,
or reordered frames. It defaults to the receiver's 256 Msps rate, a 65536-point
Hann-window FFT, and the first 2048 frames. Use `--max-frames 0` to process all
input data and `--sideband lower` when RF frequency decreases with baseband
frequency. Pass rotated parts in chronological order to combine them.

For old files whose payload remains in FPGA byte order—8-bit imaginary (Q)
followed by 8-bit real (I)—use the dedicated reader:

```sh
python scripts/plot_old_vdif_spectrum.py old.vdif \
  --center-frequency-hz 512e6
```

When `-o/--output` is omitted, both VDIF spectrum tools write a self-contained
`.spectrum.html` plot. Open it in a browser to zoom the frequency axis with the
mouse wheel, drag to pan, and hover to inspect frequency and power. No network
connection or JavaScript package download is required. Use an explicit `.svg`,
`.pdf`, or `.png` suffix when a static plot is needed.

The old-format reader treats components as offset binary by default (`0x80` is
zero). If an old file already contains signed two's-complement bytes, add
`--encoding twos-complement`. The old recorder stored the 8192-byte payload
length, rather than the 8224-byte header-plus-payload length required by VDIF,
in the frame-length field. This reader applies that historical convention and
advances 32 + 8192 bytes per frame while still validating the remaining header
format.

## ON/OFF spectral calibration

Install the offline spectral-processing dependencies and convert an ON/OFF
SDFITS pair to a single CLASS-readable FITS spectrum with an LSRK radio-velocity
axis:

```sh
python -m pip install -r scripts/requirements.txt
python scripts/make_class_onoff.py on.sdfits off.sdfits output.fits \
  --ra 02:27:03.866 --dec +61:52:24.82 --rest-mhz 22235.080 --plot
```

By default the script reads `OBSFREQ` and `CHAN_BW` from the Recorder output,
normalizes the accumulated powers by total exposure, calibrates XX and YY
separately, and then averages them into Stokes I. A three-channel Hanning kernel
is applied by default; use
`--smooth none`, `--smooth boxcar --smooth-width 5`, or
`--smooth gaussian --smooth-sigma 2` to change it. Use `--tsys-k` only when an
independently measured system temperature is available. Run
`python scripts/make_class_onoff.py --help` for manual LO/IF frequency setup,
sideband, and single-polarization options.

When the SDFITS rows contain the Recorder `NOISE` state, rows with `NOISE=0`
are used for the source spectrum and rows with `NOISE=1` are used only for
noise-diode calibration (`NOISE=2` is discarded). Given the measured diode
temperature, produce a spectrum in kelvin with:

```sh
python scripts/make_class_onoff.py on.sdfits off.sdfits output.fits \
  --tcal-k 5.0
```

Use `--tcal-xx-k` and `--tcal-yy-k` instead when the two receptor calibration
temperatures differ. The script derives the OFF-scan system temperature per
channel as `Tsys = Tcal * Poff / (Pcal_on - Poff)` and then applies the usual
position-switch calibration `Ta = ((Pon - Poff) / Poff) * Tsys`. Tcal must be
the effective injected calibration temperature for this receiver and observing
frequency; it is not inferred from the switching state alone.

See [`docs/sdfits-onoff.md`](docs/sdfits-onoff.md) for the complete calibration,
frequency-axis, metadata, and CLASS import workflow.

## Local SDFITS copy

Spectral-line mode can also save completed integrations on each processing
node while continuing to transmit them to `Spectral_Line_Recorder`. Enable the
`local_sdfits` block in `config.yaml`; local files use the same SDFITS template,
binary-table columns, TDIM order, Stokes payload, noise-state field, and
filename convention as Recorder output. See
[`docs/local-sdfits.md`](docs/local-sdfits.md) for configuration and operational
details.

Developer: YAO GUO, Institute of Automation, Chinese Academy of Sciences.
