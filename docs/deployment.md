# Deployment guide

## Scope and assumptions

This program is a Linux-only, hardware-coupled receiver. The current code
expects two DPDK-visible NIC ports, eight RX queues per port, CUDA-capable GPUs,
and VDIF UDP input with a 32-byte VDIF header plus an 8192-byte payload. Test
with a non-production NIC and data destination first: the program installs a
catch-all DPDK drop rule on each input port.

The runtime layout is fixed in the current source: two physical 100G DPDK
ports, eight RX queues per port, and eight subbands with X/Y polarisation
streams. Each 100G port independently receives the complete UDP destination
port range `60000` through `60007`. Adjacent UDP ports form one subband: the
even-numbered port carries X polarisation and the following odd-numbered port
carries Y polarisation.

| Fixed input UDP destination port | RX queue | Polarisation | Subband on DPDK port 0 | Subband on DPDK port 1 |
|---:|---:|:---:|---:|---:|
| 60000 | 0 | X | 0 | 4 |
| 60001 | 1 | Y | 0 | 4 |
| 60002 | 2 | X | 1 | 5 |
| 60003 | 3 | Y | 1 | 5 |
| 60004 | 4 | X | 2 | 6 |
| 60005 | 5 | Y | 2 | 6 |
| 60006 | 6 | X | 3 | 7 |
| 60007 | 7 | Y | 3 | 7 |

Packets arriving on other UDP destination ports are dropped by the catch-all
DPDK flow rule. Spectrum results leave through a kernel-managed SR-IOV virtual
function (VF) selected by the Linux routing table. Every configured
`subbands[].port` value (`60000–60007` in the example configuration) is the
outgoing result destination ZeroMQ/TCP port on `Storage_node_ip`. It is
independent of the fixed 100G input-port mapping above and is not a local
source port. The sender does not install a static ARP entry.

## Host prerequisites

- Linux with a C++17 compiler, Meson and Ninja.
- NVIDIA driver and CUDA toolkit compatible with the target GPU. The supplied
  build file uses `-arch=sm_86`; change this for other GPU architectures.
- DPDK development headers/libraries, `spdlog`, `fmt`, `yaml-cpp`, and
  `libzmq`.
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
pkg-config --modversion libdpdk spdlog fmt yaml-cpp libzmq
meson --version
```

## Prepare DPDK safely

1. Reserve huge pages according to the DPDK documentation and mount hugetlbfs.
2. Record the NIC's PCI addresses and current driver using
   `dpdk-devbind.py --status`.
3. Bind only the dedicated receive NIC ports to `vfio-pci`. Do not bind the
   kernel-managed result-transmission VF, or the NIC carrying the SSH session,
   to DPDK.
4. Give the service account access to `/dev/vfio/*`, huge pages, and enough
   `memlock` allowance; alternatively run under a controlled service with the
   required capabilities.
5. Verify that DPDK sees both ports before starting the receiver.

NIC binding changes network ownership. Keep console or out-of-band access
available while performing this step.

## Build

Run from the repository root:

```sh
rm -rf build
meson setup build
ninja -C build
```

The executable is `build/7mm`. If CUDA is not installed under
`/usr/local/cuda/lib64`, adjust the `cudart`/`cufft` lookup paths in
`meson.build` instead of copying libraries.

## Deploy and build multiple servers with Ansible

The repository includes `ansible/deploy.yml` for packaging the controller's
current source tree, distributing it to the `spectral_line_servers` inventory
group, preparing each host, and compiling the program. Targets execute
independently, so a slow build does not hold up other hosts.

Copy and edit the inventory, verify SSH connectivity, and run the playbook:

```sh
cp ansible/inventory.example.yml ansible/inventory.yml
ansible -i ansible/inventory.yml spectral_line_servers -m ping
ansible-playbook -i ansible/inventory.yml ansible/deploy.yml --forks 10
```

To update and compile code on servers that have already been deployed, without
repeating package installation or service provisioning, use:

```sh
ansible-playbook -i ansible/inventory.yml ansible/deploy.yml --forks 10 \
  -e spectral_line_update_only=true
```

This mode preserves the remote `config.yaml` and `.venv`, stops the receiver,
deletes the previous `build` directory, creates a clean Meson build, compiles
it with `ninja -C build`, verifies `build/7mm`, and skips dependency
installation, desktop settings, systemd unit installation, monitor changes,
and summary generation. It leaves the receiver stopped for coordinated cluster
startup. Full deployments also remove the previous build directory before
running `meson setup build` and `ninja -C build`.

The playbook stops the graphical display manager on every receiver and changes
the default boot target to `multi-user.target`. This leaves SSH, networking,
and system services running while preventing desktop sessions from consuming
receiver resources. Run the same operation without a rebuild with:

```sh
ansible-playbook -i ansible/inventory.yml ansible/disable-desktop.yml
```

Full deployment also installs and applies
`/etc/sysctl.d/99-spectral-line.conf`. To update only the persistent kernel
and network tuning on an existing cluster, run:

```sh
ansible-playbook -i ansible/inventory.yml ansible/configure-sysctl.yml \
  --forks 10
```

The final configured `net.core.netdev_max_backlog` is `250000`.
`net.ipv4.tcp_low_latency` is included only on kernels that expose its
`/proc/sys` node.

To restore desktop startup on a host, run `systemctl set-default
graphical.target` followed by `systemctl start display-manager.service`.

The playbook installs the non-DPDK apt build dependencies, deploys the
controller's local source under `/opt/Spectral_line`, produces
`/opt/Spectral_line/build/7mm`, creates the monitor virtual environment, and
enables and starts the monitor systemd service. The receiver service remains
disabled and stopped so that the cluster can be started in a coordinated way.
On subsequent deployments it stops both services before the update, restarts
the monitor, and leaves the receiver stopped. For each host, it sets `ServerID`
in the deployed
`config.yaml` from the inventory variable `spectral_line_server_id`, or from
the trailing digits of the inventory hostname when the variable is omitted.
The resulting ID must be between 0 and 7. After deployment it also generates
`ansible/deployment-summary.md` on the controller with separate tables for each
receiver's management IP, local BMC IP, 100G receiver addresses, live local
SR-IOV sender address, configured storage destination, and subband frequency
ranges. The local sender address is queried from the operating-system interface
named by `Sender_Nic`; it is not taken from `Storage_node_ip`. The BMC IP is read with
`ipmitool lan print` (channel 1 by default, configurable with
`spectral_line_bmc_channel`). The 100G entries are discovered by mapping `mlx5_0`
and `mlx5_1` to Linux interfaces with `ibdev2netdev`, then reading each
interface's IPv4 and MAC address. A per-host `spectral_line_100g_interfaces`
inventory list can supply fallback values when discovery is unavailable. Run
`ansible-playbook -i ansible/inventory.yml ansible/summary.yml` to refresh the
summary without redeploying.

Start or stop the receiver on every inventory host from the controller:

```sh
ansible -i ansible/inventory.yml spectral_line_servers --become \
  -m systemd -a "name=spectral-line.service state=started"
ansible -i ansible/inventory.yml spectral_line_servers --become \
  -m systemd -a "name=spectral-line.service state=stopped"
```

These commands do not enable the receiver at boot.

The playbook deliberately does not install or upgrade DPDK and does not manage
GRUB, kernel command-line options, huge pages, IOMMU, NIC bindings, or host
reboots. Install the DPDK runtime and development files and prepare the other
DPDK prerequisites separately before running it. The deployment fails at its
preflight check if `pkg-config` cannot find `libdpdk`. See `ansible/README.md`
for variables, password authentication, and operational details.

## Configure

Copy and edit `config.yaml` for the observing setup. Important fields:

- `observation_mode`: `0` baseband recording, `1` spectral line, `2` continuum.
- `Memory_pool_per_stream`: pinned/RAM pool in GiB. Total allocation scales with
  enabled streams; start conservatively.
- `integration_t`, `win_bw`, and `win_channels`: determine FFT length and
  integration. The computed FFT length must be a multiple of 4096 and at least
  65536.
- `subbands[].windows[].center_freq`: requested center frequency for each
  spectral window. All windows use the global `win_bw` and `win_channels`.
  Frequencies are calculated in double precision and the requested window is
  aligned to the nearest FFT channel. At startup, the receiver logs the actual
  output interval as `[start, end)` for every window. The spectrum header stores
  the actual center frequency of channel 0 in `start_freq_hz`; consumers derive
  the exclusive cutoff as `start_freq_hz + n_channels * channel_bw_hz` and the
  final channel center as `start_freq_hz + (n_channels - 1) * channel_bw_hz`.
- `subbands[].beam`: beam label `A` or `B`. Each subband can select its beam
  independently. For compatibility with an older deployed `config.yaml`, a
  missing value defaults to A for subbands 0–3 and B for subbands 4–7 with a
  startup warning. Spectrum protocol version 2 writes this value to
  `spectrum_header.beam_id`, where `0` means beam A and `1` means beam B. The
  field reuses one byte of the former two-byte reserved area, so the packed
  header remains 95 bytes and offsets of all preceding fields remain unchanged.
  Adjacent A/B configuration entries share one physical subband ID. The sender
  calculates the global `spectrum_header.subband_id` as
  `ServerID * 4 + local_config_index / 2`, producing IDs 0 through 31 across
  GPU0 through GPU7; `beam_id` keeps A and B distinct.
- `Storage_node_ip`: address of the result receiver. Linux routing selects the
  output interface and performs normal ARP/neighbor discovery.
- `Storage_node_mac` and `Sender_Nic`: optional deployment-summary metadata;
  the ZeroMQ sender does not read them or install a permanent neighbor entry.
- `subbands[].port`: destination ZeroMQ/TCP port on `Storage_node_ip` for that
  subband's complete spectrum results. It does not configure a local source
  port. Port values do not identify subbands, beams, or windows; the Writer
  reads those identities from `spectrum_header`. Multiple subbands may share a
  destination port. The Writer's `result_ports` list only declares the unique
  TCP ports on which it listens, and its order has no meaning.
- `Baseband_Folder0`/`Baseband_Folder1`: writable high-throughput filesystems
  used only in baseband mode.
- `Baseband_bits`: `8`, `4`, or `2`. The program converts each FPGA
  offset-binary component to VDIF two's-complement coding. For 4-bit and 2-bit
  output it then retains the most-significant bits and packs respectively two
  or four components per byte. FPGA complex samples arrive in Q,I order and
  are written in the VDIF-standard I,Q order.
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

## Install the receiver as a systemd service

The supplied unit runs the receiver from `/opt/Spectral_line`, where it finds
`config.yaml` and writes its log files. Install the unit after compiling:

```sh
sudo install -m 0644 systemd/spectral-line.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl disable --now spectral-line.service
```

This installs the receiver without enabling it at boot. Start it explicitly
only when the complete receiver cluster is ready.

From the controller, start or stop the receiver on every inventory host with:

```sh
bash ansible/service-control.sh start
bash ansible/service-control.sh stop
```

The same script accepts `restart` and `status`. It changes the current runtime
state while keeping `spectral-line.service` disabled at boot.

Control and inspect the receiver with standard systemd commands:

```sh
sudo systemctl stop spectral-line.service
sudo systemctl restart spectral-line.service
sudo systemctl status spectral-line.service
sudo journalctl -u spectral-line.service -f
```

`systemctl status` shows the service state and recent log lines. Use
`journalctl -u spectral-line.service` for the complete journal, `-f` to follow
new messages, or `--since today` to limit the time range. Receiver logs are
written to both the systemd journal and timestamped `log_*.log` files under
`/opt/Spectral_line`. Systemd does not automatically restart `7mm`; the
receiver process is expected to remain active until an explicit
`systemctl stop spectral-line.service` terminates it. An unexpected exit is
therefore visible as a failed or inactive service instead of a restart loop.

The receiver unit runs as root because DPDK/VFIO access and the current static
neighbor setup require elevated privileges. Do not start it until the required
GRUB reboot, huge pages, NIC binding, and `config.yaml` checks are complete.

## Start and validate

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
`/dev/shm/server_<ServerID>_stream_<stream>.bin`. A file is created only after
its stream receives a valid packet; a subband with no input does not produce a
misleading empty monitor file. Files left by an earlier receiver process are
removed during the next receiver initialization.

## Install the monitor as a systemd service

The monitor runs continuously with no command-line configuration. The supplied
unit expects the checkout and virtual environment under `/opt/Spectral_line`
and a service account named `spectral-line`:

```sh
sudo useradd --system --home-dir /opt/Spectral_line --shell /usr/sbin/nologin spectral-line
sudo python3 -m venv /opt/Spectral_line/.venv
sudo /opt/Spectral_line/.venv/bin/pip install -r /opt/Spectral_line/monitor/requirements.txt
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
MONITOR_PLOT_WORKERS=4
MONITOR_PLOT_INTERVAL_SECONDS=2
```

The monitor accepts updates from all 16 stream files in one scan and submits
them to the process pool together. The built-in default is the smaller of 16
and the host's logical CPU count. Lower this value if monitor rendering affects
the CPU cores reserved for DPDK packet reception.

After every successful batch, `/dev/shm/monitor_plots/manifest.json` is replaced
atomically. If any changed stream fails to render, the previous manifest stays
active and the changed streams are retried during the next scan.

Monitor images are JPEG files named
`<subband>_<beam>_<polarization>_<type>_<quality>.jpg`. The global subband
number is `ServerID * 4 + stream_id / 4`, producing IDs 0 through 31 across
GPU0 through GPU7. Beam A/B and polarization X/Y are derived from the fixed
input stream order. Type `1` is raw ADC, type `2` is the
normal-distribution histogram, and type `3` is the frequency spectrum.
High-resolution files end in `_hq.jpg`; thumbnails end in `_lq.jpg`.
Both qualities are written directly to `/dev/shm/monitor_plots/`; no
`high_resolution` or `thumbnails` subdirectories are used.

After updating the repository, refresh the environment and restart only the
monitor service:

```sh
sudo /opt/Spectral_line/.venv/bin/pip install -r /opt/Spectral_line/monitor/requirements.txt
sudo systemctl restart spectral-line-monitor.service
sudo journalctl -u spectral-line-monitor.service -f
```

The monitor's standard output and errors are also stored in the systemd
journal under `spectral-line-monitor.service`.

## Operational cautions

- The main processing and receive loops run indefinitely. Stop them through
  `systemctl stop spectral-line.service`; systemd terminates the complete
  process control group if the receiver does not exit within the stop timeout.
- CUDA host allocation and DPDK huge-page allocation can fail independently;
  monitor both host memory and GPU memory.
- Test a configuration change with recorded traffic before applying it to a
  live observation.
