# Deployment guide

## Scope and assumptions

This program is a Linux-only, hardware-coupled receiver. The current code
expects two DPDK-visible NIC ports, eight RX queues per port, CUDA-capable GPUs,
and VDIF UDP input with a 32-byte VDIF header plus an 8192-byte payload. Test
with a non-production NIC and data destination first: the program installs a
catch-all DPDK drop rule on each input port.

The runtime layout is fixed in the current source: two physical DPDK ports,
eight queues per port, and up to eight subbands (two polarisation streams per
subband). Spectrum results leave through a kernel-managed SR-IOV virtual
function (VF), selected in `Sender_Nic`. Each configured `subbands[].port` is
the destination UDP port on `Storage_node_ip`; it is not the local source port.
Input flow rules currently listen on UDP ports 60000 through 60007.

## Host prerequisites

- Linux with a C++17 compiler, Meson and Ninja.
- NVIDIA driver and CUDA toolkit compatible with the target GPU. The supplied
  build file uses `-arch=sm_86`; change this for other GPU architectures.
- DPDK development headers/libraries, `spdlog`, `fmt`, and `yaml-cpp`.
- Two receive NIC ports supported by DPDK and bound to a userspace driver such
  as `vfio-pci`; huge pages and sufficient locked memory for DPDK/CUDA pinned
  buffers.
- A separate SR-IOV VF for spectrum-result transmission. It must remain bound
  to its Linux kernel driver and have connectivity to the storage node.
- Disk capacity and write bandwidth appropriate for baseband mode. Baseband
  files are rotated at 8 GiB per stream.

Example package names vary by distribution. On Debian/Ubuntu, install the build
dependencies first, then install CUDA and a DPDK version compatible with the
NIC driver. Confirm them before building:

```sh
nvidia-smi
dpdk-testpmd --version
pkg-config --modversion libdpdk spdlog fmt yaml-cpp
meson --version
```

## Prepare DPDK safely

1. Reserve huge pages according to the DPDK documentation and mount hugetlbfs.
2. Record the NIC's PCI addresses and current driver using
   `dpdk-devbind.py --status`.
3. Bind only the dedicated receive NIC ports to `vfio-pci`. Do not bind the
   SR-IOV VF named by `Sender_Nic`, or the NIC carrying the SSH session, to
   DPDK.
4. Give the service account access to `/dev/vfio/*`, huge pages, and enough
   `memlock` allowance; alternatively run under a controlled service with the
   required capabilities.
5. Verify that DPDK sees both ports before starting the receiver.

NIC binding changes network ownership. Keep console or out-of-band access
available while performing this step.

## Build

Run from the repository root:

```sh
meson setup build --buildtype=release
meson compile -C build
```

The executable is `build/7mm`. If CUDA is not installed under
`/usr/local/cuda/lib64`, adjust the `cudart`/`cufft` lookup paths in
`meson.build` instead of copying libraries.

## Configure

Copy and edit `config.yaml` for the observing setup. Important fields:

- `observation_mode`: `0` baseband recording, `1` spectral line, `2` continuum.
- `Memory_pool_per_stream`: pinned/RAM pool in GiB. Total allocation scales with
  enabled streams; start conservatively.
- `integration_t`, `win_bw`, and `win_channels`: determine FFT length and
  integration. The computed FFT length must be a multiple of 4096 and at least
  65536.
- `Storage_node_ip` and `Storage_node_mac`: address of the result receiver.
- `Sender_Nic`: name of the kernel-managed SR-IOV VF used to reach the result
  receiver; the example configuration uses `ens81f0v0`. The current code uses
  this name when installing the permanent neighbour entry. Linux routing must
  also select this VF for `Storage_node_ip`.
- `subbands[].port`: destination UDP port on `Storage_node_ip` for that
  subband's spectrum results. It does not configure a local source port.
- `Baseband_Folder0`/`Baseband_Folder1`: writable high-throughput filesystems
  used only in baseband mode.
- `Baseband_bits`: `8`, `4`, or `2`. For 4-bit and 2-bit output, the program
  retains the most-significant bits of each original 8-bit component and packs
  them respectively two or four samples per byte.
- `noise_source.duty_cycle`: percentage in the open interval `(0, 100)`.
  A value of `50` means 50% of each configured period is ON.

Ensure every enabled subband maps to an available GPU and that all configured
spectrum windows lie within its subband.

Before starting the receiver, verify the result path (substitute the configured
address and interface if they differ):

```sh
ip link show ens81f0v0
ip route get 192.168.101.3
ip neigh show 192.168.101.3 dev ens81f0v0
```

## Start and validate

Run from the directory containing `config.yaml`:

```sh
./build/7mm
```

Before an observation, verify:

1. The startup log reports all enabled subband threads ready.
2. DPDK flow creation succeeds for the expected UDP ports.
3. Packet-loss counters remain at an acceptable level under representative
   traffic.
4. Spectral output reaches the storage node, or baseband files grow at the
   expected rate.
5. For 8-bit baseband output, validate a produced file with `m5test`. Validate
   4-bit/2-bit output with a compatible VDIF decoder that uses the header's
   bits-per-sample field.

Logs are written to `log_YYYY-MM-DD_HH-MM-SS.log` in the working directory.
The optional subband monitor publishes a recent VDIF frame to
`/dev/shm/server_<ServerID>_stream_<stream>.bin`.

## Install the monitor as a systemd service

The monitor runs continuously with no command-line configuration. The supplied
unit expects the checkout and virtual environment under `/opt/Spectral_line`
and a service account named `spectral-line`:

```sh
sudo useradd --system --home-dir /opt/Spectral_line --shell /usr/sbin/nologin spectral-line
sudo python3 -m venv /opt/Spectral_line/.venv
sudo /opt/Spectral_line/.venv/bin/pip install -r /opt/Spectral_line/requirements-monitor.txt
sudo install -m 0644 systemd/spectral-line-monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now spectral-line-monitor.service
```

The DPDK process creates monitor files with mode `0666`; ensure the active
umask still permits the service account to read them. `/dev/shm` is recreated
at boot, so the unit's `ExecStartPre` creates `/dev/shm/monitor_plots` with the
correct service-account ownership each time it starts.

To change concurrency without editing the unit, create
`/etc/default/spectral-line-monitor`:

```sh
MONITOR_PLOT_WORKERS=16
```

The monitor accepts updates from all 16 stream files in one scan and submits
them to the process pool together. The built-in default is the smaller of 16
and the host's logical CPU count. Lower this value if monitor rendering affects
the CPU cores reserved for DPDK packet reception.

After every successful batch, `/dev/shm/monitor_plots/manifest.json` is replaced
atomically. If any changed stream fails to render, the previous manifest stays
active and the changed streams are retried during the next scan.

After updating the repository, refresh the environment and restart only the
monitor service:

```sh
sudo /opt/Spectral_line/.venv/bin/pip install -r /opt/Spectral_line/requirements-monitor.txt
sudo systemctl restart spectral-line-monitor.service
sudo journalctl -u spectral-line-monitor.service -f
```

## Operational cautions

- The main processing and receive loops run indefinitely. Use a supervisor and
  a defined shutdown procedure for production observations.
- CUDA host allocation and DPDK huge-page allocation can fail independently;
  monitor both host memory and GPU memory.
- Test a configuration change with recorded traffic before applying it to a
  live observation.
