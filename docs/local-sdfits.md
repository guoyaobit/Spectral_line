# Local SDFITS output

The spectral-line processing program can save each completed integration to
local SDFITS while continuing to send the same result to
`Spectral_Line_Recorder`. Local writing is asynchronous: GPU workers copy a
completed result into a bounded queue and a dedicated CPU thread performs the
CFITSIO operations.

## Configuration

Enable this block in `config.yaml`:

```yaml
observation_mode: 1
Observation_ID: 20260920T011500Z_0033+636_ON

local_sdfits:
  enabled: true
  storage_folder: /data
  object: 0033+636
  source_on: true
  queue_mb: 2048
```

`Observation_ID` should be supplied by the observation controller and should
be identical on every processing node and on the Recorder. The local directory
is `<storage_folder>/<Observation_ID>/`. `storage_folder` defaults to `/data`
when omitted, so the default output is `/data/<Observation_ID>/`.

If `Observation_ID` is omitted, the processing node creates a UTC directory
name locally. This is suitable for manual tests, but its creation time may not
exactly match the directory independently created by the Recorder.

`source_on` controls the `ON` or `OFF` filename component. Set it to `false`
for the reference-sky scan. `queue_mb` bounds pending local data; when the disk
cannot keep up and the queue becomes full, the processing stream continues and
the omitted local row is reported in the log.

## Format compatibility

The local writer uses the same `sdfits` implementation and
`vegas_SDFITS_template.txt` schema as `Spectral_Line_Recorder`. Consequently,
the following are identical between local and Recorder output:

- primary HDU and `SINGLE DISH` binary-table extension;
- keyword names and units;
- 21 table columns, including `NOISE`;
- `DATA` element type and `(NCHAN, NSUBBAND, NPOL, 1, 1)` TDIM order;
- one row per integration and four-float Stokes payload per channel;
- output filename convention.

The files need not be byte-for-byte identical because their creation
timestamps and row arrival histories can differ. Their scientific data layout
is the same, so the same Python, Astropy, and GILDAS/CLASS workflow can read
either file.

The program must be started from the installation directory containing
`vegas_SDFITS_template.txt`, matching the Recorder's current runtime
requirement.
