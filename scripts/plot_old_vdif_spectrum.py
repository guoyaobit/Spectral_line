#!/usr/bin/env python3
"""Plot spectra from the receiver's old VDIF Q,I byte layout.

Each frame has a standard 32-byte non-legacy VDIF header.  The payload is
stored as repeating 8-bit Q (imaginary), I (real) component pairs.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
from collections.abc import Sequence

import numpy as np

from plot_vdif_spectrum import (
    DEFAULT_MAX_FRAMES,
    DEFAULT_NFFT,
    DEFAULT_SAMPLE_RATE_HZ,
    ReadStats,
    SpectrumResult,
    _format_summary,
    _window,
    iter_vdif_frames,
    plot_spectrum,
    write_csv,
)


def decode_old_qi_payload(
    payload: bytes, encoding: str = "offset-binary"
) -> np.ndarray:
    """Convert Q,I,Q,I bytes to complex64 samples in I + jQ form."""
    packed = np.frombuffer(payload, dtype=np.uint8)
    if packed.size % 2:
        raise ValueError("old Q,I payload has an odd byte count")

    pairs = packed.reshape(-1, 2)
    if encoding == "offset-binary":
        # FPGA zero level is 0x80.  Subtraction must happen after widening.
        q_values = pairs[:, 0].astype(np.float32) - 128.0
        i_values = pairs[:, 1].astype(np.float32) - 128.0
    elif encoding == "twos-complement":
        signed = packed.view(np.int8).astype(np.float32).reshape(-1, 2)
        q_values = signed[:, 0]
        i_values = signed[:, 1]
    else:
        raise ValueError(
            f"unknown sample encoding {encoding!r}; expected offset-binary "
            "or twos-complement"
        )

    return (i_values + 1j * q_values).astype(np.complex64)


def calculate_old_spectrum(
    paths: Sequence[pathlib.Path],
    sample_rate_hz: float = DEFAULT_SAMPLE_RATE_HZ,
    nfft: int = DEFAULT_NFFT,
    max_frames: int | None = DEFAULT_MAX_FRAMES,
    frames_per_second: int = 62500,
    window_name: str = "hann",
    encoding: str = "offset-binary",
) -> SpectrumResult:
    if not paths:
        raise ValueError("at least one input VDIF file is required")
    if sample_rate_hz <= 0:
        raise ValueError("sample rate must be positive")
    if frames_per_second <= 0:
        raise ValueError("frames per second must be positive")
    if nfft < 2 or nfft & (nfft - 1):
        raise ValueError("FFT size must be a power of two greater than one")
    if max_frames is not None and max_frames <= 0:
        raise ValueError("max frames must be positive or omitted")

    stats = ReadStats()
    window = _window(window_name, nfft)
    window_power = float(np.sum(window * window))
    accumulated = np.zeros(nfft, dtype=np.float64)
    pending = np.empty(0, dtype=np.complex64)
    averages = 0
    first_header = None

    for header, payload in iter_vdif_frames(
        paths,
        stats,
        frames_per_second,
        max_frames,
        frame_length_includes_header=False,
    ):
        if header.bits_per_sample != 8:
            raise ValueError(
                f"old Q,I reader requires 8-bit components; VDIF header "
                f"declares {header.bits_per_sample} bits"
            )
        if first_header is None:
            first_header = header

        samples = decode_old_qi_payload(payload, encoding) / 128.0
        pending = np.concatenate((pending, samples))
        while pending.size >= nfft:
            block = pending[:nfft]
            pending = pending[nfft:]
            transformed = np.fft.fft(block * window)
            accumulated += np.abs(transformed) ** 2
            averages += 1

    if first_header is None:
        raise ValueError("no valid VDIF frames were found")
    if averages == 0:
        raise ValueError(
            f"not enough samples for one {nfft}-point FFT; "
            f"read {stats.frames_used} valid frame(s)"
        )

    psd = accumulated / (averages * sample_rate_hz * window_power)
    frequency_hz = np.fft.fftshift(
        np.fft.fftfreq(nfft, d=1.0 / sample_rate_hz)
    )
    psd = np.fft.fftshift(psd)
    psd_dbfs_hz = 10.0 * np.log10(
        np.maximum(psd, np.finfo(np.float64).tiny)
    )
    return SpectrumResult(
        frequency_hz=frequency_hz,
        psd_dbfs_hz=psd_dbfs_hz,
        averages=averages,
        bits_per_sample=8,
        thread_id=first_header.thread_id,
        stats=stats,
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Read old-format VDIF frames whose 8-bit payload bytes alternate "
            "Q (imaginary), I (real), then plot an averaged power spectrum."
        )
    )
    parser.add_argument("input", nargs="+", type=pathlib.Path)
    parser.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        help="output plot; use .svg or .pdf for lossless zoom",
    )
    parser.add_argument("--csv", type=pathlib.Path, help="also write spectrum CSV")
    parser.add_argument(
        "--encoding",
        choices=("offset-binary", "twos-complement"),
        default="offset-binary",
        help=(
            "8-bit component encoding (default: offset-binary, where 0x80 "
            "represents zero)"
        ),
    )
    parser.add_argument(
        "--sample-rate-hz", type=float, default=DEFAULT_SAMPLE_RATE_HZ
    )
    parser.add_argument("--nfft", type=int, default=DEFAULT_NFFT)
    parser.add_argument(
        "--max-frames",
        type=int,
        default=DEFAULT_MAX_FRAMES,
        help=(
            f"maximum frames to read (default: {DEFAULT_MAX_FRAMES}; "
            "use 0 for all frames)"
        ),
    )
    parser.add_argument("--frames-per-second", type=int, default=62500)
    parser.add_argument(
        "--window", choices=("hann", "rectangular"), default="hann"
    )
    parser.add_argument("--center-frequency-hz", type=float)
    parser.add_argument(
        "--sideband", choices=("upper", "lower"), default="upper"
    )
    parser.add_argument("--title")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    missing = [str(path) for path in args.input if not path.is_file()]
    if missing:
        parser.error("input file not found: " + ", ".join(missing))

    output = args.output or args.input[0].with_suffix(".spectrum.png")
    max_frames = None if args.max_frames == 0 else args.max_frames
    try:
        result = calculate_old_spectrum(
            args.input,
            sample_rate_hz=args.sample_rate_hz,
            nfft=args.nfft,
            max_frames=max_frames,
            frames_per_second=args.frames_per_second,
            window_name=args.window,
            encoding=args.encoding,
        )
        plot_spectrum(
            result,
            output,
            args.center_frequency_hz,
            args.sideband,
            args.title or "VDIF Q,I baseband spectrum",
        )
        if args.csv:
            write_csv(
                args.csv,
                result,
                args.center_frequency_hz,
                args.sideband,
            )
    except (OSError, ValueError) as error:
        parser.error(str(error))

    print(_format_summary(result))
    print(f"sample layout=Q,I; encoding={args.encoding}")
    print(f"wrote {output}")
    if args.csv:
        print(f"wrote {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
