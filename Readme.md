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
