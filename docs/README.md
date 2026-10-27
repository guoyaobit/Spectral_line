# Project documentation

## Current implementation

- `server1` through `server8` correspond to GPU0 through GPU7, with `ServerID`
  0 through 7. These identify servers, not the local CUDA `gpu_id`.
- The supplied YAML files use the allocation workbook's RF frequencies in Hz.
  The root `config.yaml` defaults to GPU0/server1. Each node processes four
  256 MHz frequency bands, each with beams A/B and two polarization inputs.
- Spectral output remains four accumulated backend products per channel:
  XX, YY, Re(X)*Im(Y), Im(X)*Re(Y). No selectable `output_type` is implemented.
  These last two products are not complete complex cross-correlation or Stokes U/V.
- Results use the 24-byte transport header and 103-byte version-3 spectrum
  header over ZeroMQ/TCP. Input UDP ports and result TCP ports are independent.
- New local and Recorder SDFITS files use `FREQTYPE=RF`. The writers do not
  convert IF to RF. Historical files retain their original frequency metadata.
- Local SDFITS is disabled by default. Its 2048 MiB queue is shared across all
  local subbands/windows; each band/beam/window has its own output file.
- Raw powers are uncalibrated. The reduction script performs ON/OFF and optional
  noise-diode calibration and smoothing, but does not subtract a fitted baseline.

## Guides

| Document | Contents |
|---|---|
| [Deployment](deployment.md) | Hardware assumptions, server/RF mapping, build, configuration and receiver services |
| [Allocation workbook](数据分配新.xlsx) | Original server, input-port, IF and RF allocation reference |
| [Ansible](../ansible/README.md) | Multi-server installation, static YAML deployment, summary and service control |
| [Result data format](result-data-format.md) | Message boundaries, headers, timestamps, products and payload sizes |
| [Local SDFITS](local-sdfits.md) | Local disk queue, metadata, DATA layout and Recorder compatibility |
| [ON/OFF reduction](sdfits-onoff.md) | Calibration, RF versus historical IF, smoothing, Python plots and CLASS export |

The separate `Spectral_Line_Recorder` repository contains its own `README.md`,
`SDFITS_REDUCTION.md` and `systemd/spectral-line-recorder.service`. Its systemd
unit uses the default root account, no startup file checks, and `Restart=no`.
It has no graceful queue-draining shutdown; stop senders before stopping Recorder.
See that repository for installation paths and commands.

## Before an observation

1. Deploy the correct GPU YAML to each server, not the root GPU0 default everywhere.
2. Confirm RF ranges, window placement, input-device order and Recorder result ports.
3. Set the actual object, ON/OFF state, pointing and surveyed site metadata.
4. Share one `Observation_ID` across processing nodes and Recorder when using a controller.
5. Check storage mounts and permissions, runtime logs, packet loss and output headers.
6. Rebuild both programs after writer source changes; copying YAML alone does not
   change the file-header code in an existing executable.
