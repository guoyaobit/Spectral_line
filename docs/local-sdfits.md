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
object: 0033+636
source_on: true

local_sdfits:
  enabled: true
  storage_folder: /data
  queue_mb: 2048
```

`Observation_ID` should be supplied by the observation controller and should
be identical on every processing node and on the Recorder. The local directory
is `<storage_folder>/<Observation_ID>/`. `storage_folder` defaults to `/data`
when omitted, so the default output is `/data/<Observation_ID>/`.

If `Observation_ID` is omitted, the processing node creates a UTC directory
name locally. This is suitable for manual tests, but its creation time may not
exactly match the directory independently created by the Recorder.

The top-level `object` and `source_on` keys match Recorder. `source_on`
controls the `ON` or `OFF` filename component and SDFITS `SRCSTATE`. Set it to `false`
for the reference-sky scan. `queue_mb` bounds pending local data; when the disk
cannot keep up and the queue becomes full, the processing stream continues and
the omitted local row is reported in the log.

## Format compatibility

DATA retains its original channel/subband/product interleaving, with products
varying fastest. `DATALAY=PRODFAST` documents this unchanged storage order.
TDIM now correctly declares `(NPOL, NSUBBAND, NCHAN, 1, 1)`; product is axis 1
and frequency is axis 3. `NCHAN`, `NSUBBAND`, and `NPOL` are retained, so the
existing `make_class_onoff.py` needs no changes.

`CRPIX3=1` is fixed for every row and `CRVAL3` is the first channel centre in
Hz. `OBSFREQ` remains the backend band centre. `FREQTYPE=IF` makes explicit that
these are intermediate frequencies: provide LO/sideband information during
offline reduction to derive sky frequencies or velocities. DATA is uncalibrated accumulated power, with
unknown `TSYS` and `TCAL` left undefined. `TIME` is explicitly in MJD days with
`MJDREF=0`; it retains the project's time convention.

The product axis is `CORRPROD`, with `POLPROD1..4` identifying `XX`, `YY`,
`Re(X)*Im(Y)` and `Im(X)*Re(Y)`. The last two are backend products, not complete
standard XY/YX correlations or Stokes U/V. This remains an instrument-specific
extension, rather than a claim of universal SDFITS reader compatibility.

Configure known observing metadata at the top level of both configurations:

```yaml
sdfits_metadata:
  telescope: NSRT
  instrument: 7mm
  ra_deg: 9.1979583333
  dec_deg: 63.4839166667
  site_lon_deg: 87.10
  site_lat_deg: 43.28
  site_height_m: 2029.38
```

Coordinates above illustrate the previous NH3 pointing and site settings;
replace them with actual pointing (including the OFF scan) and surveyed site
coordinates. Omitted sky coordinates are NaN and omitted site coordinates are
not written. Source name and ON/OFF state come from the scan configuration.

Both projects use these identical names and units:

| Configuration key | SDFITS field |
| --- | --- |
| `object` | `OBJECT` |
| `source_on` | `SRCSTATE` and ON/OFF filenames |
| `sdfits_metadata.telescope` | `TELESCOP` |
| `sdfits_metadata.instrument` | `INSTRUME` |
| `sdfits_metadata.ra_deg` | `CRVAL4`, ICRS degrees |
| `sdfits_metadata.dec_deg` | `CRVAL5`, ICRS degrees |
| `sdfits_metadata.site_lon_deg` | `SITELONG`, east-positive degrees |
| `sdfits_metadata.site_lat_deg` | `SITELAT`, degrees |
| `sdfits_metadata.site_height_m` | `SITEELEV`, metres |

Processing nodes still accept old `local_sdfits.object` and
`local_sdfits.source_on` as a fallback; top-level values take precedence.

The local writer uses the same `sdfits` implementation and
`vegas_SDFITS_template.txt` schema as `Spectral_Line_Recorder`. Consequently,
the following are identical between local and Recorder output:

- primary HDU and `SINGLE DISH` binary-table extension;
- keyword names and units;
- 21 table columns, including `NOISE`;
- `DATA` element type and `(NPOL, NSUBBAND, NCHAN, 1, 1)` TDIM order;
- one row per integration and four-float backend-product payload per channel;
- output filename convention.

The files need not be byte-for-byte identical because their creation
timestamps and row arrival histories can differ. Their scientific data layout
is the same, so the same Python/Astropy reduction can read either file.
For CLASS, use the unchanged `make_class_onoff.py` ON/OFF workflow to generate
one CLASS-readable FITS spectrum. Direct CLASS import of native SDFITS is not
provided.

The program must be started from the installation directory containing
`vegas_SDFITS_template.txt`, matching the Recorder's current runtime
requirement.
