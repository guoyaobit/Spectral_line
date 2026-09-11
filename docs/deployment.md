# Deployment guide

## Scope and assumptions

This program is a Linux-only, hardware-coupled receiver. The current code
expects two DPDK-visible NIC ports, eight RX queues per port, CUDA-capable GPUs,
and VDIF UDP input with a 32-byte VDIF header plus an 8192-byte payload. Test
with a non-production NIC and data destination first: the program installs a
catch-all DPDK drop rule on each input port.

The runtime layout is fixed in the current source: two physical DPDK ports,
eight queues per port, and up to eight subbands (two polarisation streams per
subband). The configured `subbands[].port` is the output UDP port for spectral
results; input flow rules currently listen on UDP ports 60000 through 60007.

## Host prerequisites

- Linux with a C++17 compiler, Meson and Ninja.
- NVIDIA driver and CUDA toolkit compatible with the target GPU. The supplied
  build file uses `-arch=sm_86`; change this for other GPU architectures.
- DPDK development headers/libraries, `spdlog`, `fmt`, and `yaml-cpp`.
- Two receive NIC ports supported by DPDK and bound to a userspace driver such
  as `vfio-pci`; huge pages and sufficient locked memory for DPDK/CUDA pinned
  buffers.
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
3. Bind only the dedicated receive NIC ports to `vfio-pci`. Do not bind the NIC
   carrying the SSH session used to administer the server.
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
- `Storage_node_ip`, `Storage_node_mac`, `Sender_Nic`: destination and outgoing
  interface for spectral packets. Confirm these values on the deployment host.
- `Baseband_Folder0`/`Baseband_Folder1`: writable high-throughput filesystems
  used only in baseband mode.
- `Baseband_bits`: `8`, `4`, or `2`. For 4-bit and 2-bit output, the program
  retains the most-significant bits of each original 8-bit component and packs
  them respectively two or four samples per byte.
- `noise_source.duty_cycle`: percentage in the open interval `(0, 100)`.
  A value of `50` means 50% of each configured period is ON.

Ensure every enabled subband maps to an available GPU and that all configured
spectrum windows lie within its subband.

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

## Operational cautions

- The main processing and receive loops run indefinitely. Use a supervisor and
  a defined shutdown procedure for production observations.
- CUDA host allocation and DPDK huge-page allocation can fail independently;
  monitor both host memory and GPU memory.
- Test a configuration change with recorded traffic before applying it to a
  live observation.
