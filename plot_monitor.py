#!/usr/bin/env python3
"""Generate diagnostic plots from one 8-bit complex VDIF frame."""

import argparse
from pathlib import Path
import time

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np


HEADER_SIZE = 32
PAYLOAD_SIZE = 8192
SAMPLE_RATE = 256e6
HIGH_SIZE = (12, 4.5)
HIGH_DPI = 300
THUMBNAIL_SIZE = (6, 2.25)
THUMBNAIL_DPI = 100


def parse_args():
    parser = argparse.ArgumentParser(
        description=(
            "Create high-resolution and thumbnail ADC, FFT, and histogram "
            "plots for every monitor stream file in a directory."
        )
    )
    parser.add_argument(
        "directory",
        nargs="?",
        type=Path,
        default=Path("/dev/shm"),
        help="monitor directory (default: /dev/shm)",
    )
    parser.add_argument(
        "--pattern",
        default="server_*_stream_*.bin",
        help="monitor filename pattern (default: server_*_stream_*.bin)",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        help="output directory (default: <monitor_directory>/monitor_plots)",
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=1024,
        help="number of complex ADC samples shown in the waveform (default: 1024)",
    )
    return parser.parse_args()


def read_monitor_snapshot(filename, retries=5, retry_delay=0.01):
    if not filename.is_file():
        raise FileNotFoundError("Input file does not exist: %s" % filename)

    expected_size = HEADER_SIZE + PAYLOAD_SIZE
    if filename.stat().st_size < expected_size:
        raise RuntimeError(
            "File is too short for one VDIF frame: expected at least %d bytes"
            % expected_size
        )

    snapshot = None
    for _attempt in range(retries):
        with filename.open("rb") as stream:
            first = stream.read(expected_size)
        time.sleep(retry_delay)
        with filename.open("rb") as stream:
            second = stream.read(expected_size)
        if len(first) == expected_size and first == second:
            snapshot = second
            break

    if snapshot is None:
        raise RuntimeError("Monitor frame changed while being read: %s" % filename)
    if not any(snapshot[:HEADER_SIZE]):
        raise RuntimeError("Monitor stream has not received its first frame: %s" % filename)

    payload = snapshot[HEADER_SIZE:]

    if len(payload) != PAYLOAD_SIZE:
        raise RuntimeError("Invalid payload size: %d" % len(payload))

    # Payload format: Imag0, Real0, Imag1, Real1, ...
    adc = np.frombuffer(payload, dtype=np.int8)
    imag = adc[0::2]
    real = adc[1::2]
    return real, imag


def save_plot_pair(output_dir, name, draw_plot):
    high_dir = output_dir / "high_resolution"
    thumbnail_dir = output_dir / "thumbnails"
    high_dir.mkdir(parents=True, exist_ok=True)
    thumbnail_dir.mkdir(parents=True, exist_ok=True)

    destinations = (
        (high_dir / (name + ".png"), HIGH_SIZE, HIGH_DPI, False),
        (thumbnail_dir / (name + ".png"), THUMBNAIL_SIZE, THUMBNAIL_DPI, True),
    )

    saved = []
    for destination, figure_size, dpi, is_thumbnail in destinations:
        figure, axes = plt.subplots(figsize=figure_size)
        draw_plot(axes, is_thumbnail)
        figure.tight_layout()
        figure.savefig(destination, dpi=dpi, bbox_inches="tight", facecolor="white")
        plt.close(figure)
        saved.append(destination)
    return saved


def plot_adc(real, imag, samples, output_dir, prefix):
    count = min(samples, real.size)
    sample_index = np.arange(count)
    time_us = sample_index / SAMPLE_RATE * 1e6

    def draw(axes, is_thumbnail):
        axes.plot(time_us, real[:count], linewidth=0.8, label="Real")
        axes.plot(time_us, imag[:count], linewidth=0.8, alpha=0.8, label="Imag")
        axes.set_title("Raw ADC Samples")
        axes.set_xlabel("Time (us)")
        axes.set_ylabel("ADC value")
        axes.set_ylim(-132, 132)
        axes.grid(alpha=0.3)
        axes.legend(loc="upper right", ncol=2, fontsize=7 if is_thumbnail else 9)

    return save_plot_pair(output_dir, prefix + "_adc_raw", draw)


def plot_fft(real, imag, output_dir, prefix):
    signal = real.astype(np.float32) + 1j * imag.astype(np.float32)
    spectrum = np.fft.fftshift(np.fft.fft(signal))
    power = np.abs(spectrum) ** 2
    reference = max(float(np.max(power)), np.finfo(np.float32).tiny)
    power_db = 10.0 * np.log10(power / reference + 1e-12)
    frequency_mhz = (
        np.fft.fftshift(np.fft.fftfreq(signal.size, d=1.0 / SAMPLE_RATE)) / 1e6
    )

    def draw(axes, _is_thumbnail):
        axes.plot(frequency_mhz, power_db, linewidth=0.8)
        axes.set_title("FFT Spectrum")
        axes.set_xlabel("Frequency (MHz)")
        axes.set_ylabel("Relative power (dB)")
        axes.set_xlim(frequency_mhz[0], frequency_mhz[-1])
        axes.grid(alpha=0.3)

    return save_plot_pair(output_dir, prefix + "_fft_spectrum", draw)


def plot_histogram(real, imag, output_dir, prefix):
    bins = np.arange(-128.5, 128.6, 2.0)

    def draw(axes, is_thumbnail):
        axes.hist(
            real,
            bins=bins,
            density=True,
            alpha=0.55,
            label="Real",
            color="tab:blue",
        )
        axes.hist(
            imag,
            bins=bins,
            density=True,
            alpha=0.55,
            label="Imag",
            color="tab:orange",
        )
        axes.set_title("ADC Value Distribution")
        axes.set_xlabel("ADC value")
        axes.set_ylabel("Probability density")
        axes.set_xlim(-128, 127)
        axes.grid(axis="y", alpha=0.3)
        axes.legend(loc="upper right", fontsize=7 if is_thumbnail else 9)

    return save_plot_pair(output_dir, prefix + "_adc_histogram", draw)


def process_stream(filename, output_dir, samples):
    real, imag = read_monitor_snapshot(filename)
    prefix = filename.stem
    saved = []
    saved.extend(plot_adc(real, imag, samples, output_dir, prefix))
    saved.extend(plot_fft(real, imag, output_dir, prefix))
    saved.extend(plot_histogram(real, imag, output_dir, prefix))
    return saved


def main():
    args = parse_args()
    if args.samples <= 0:
        raise ValueError("--samples must be greater than zero")

    monitor_dir = args.directory.resolve()
    if not monitor_dir.is_dir():
        raise NotADirectoryError("Monitor directory does not exist: %s" % monitor_dir)

    output_dir = (
        args.output_dir.resolve()
        if args.output_dir
        else monitor_dir / "monitor_plots"
    )

    stream_files = sorted(path for path in monitor_dir.glob(args.pattern) if path.is_file())
    if not stream_files:
        raise RuntimeError(
            "No monitor stream files matching %r in %s"
            % (args.pattern, monitor_dir)
        )

    saved = []
    failed = []
    for filename in stream_files:
        try:
            saved.extend(process_stream(filename, output_dir, args.samples))
        except (OSError, RuntimeError, ValueError) as error:
            failed.append((filename, error))

    print("Processed %d of %d monitor streams." % (
        len(stream_files) - len(failed), len(stream_files)
    ))
    print("Generated %d diagnostic images in %s" % (len(saved), output_dir))
    for filename, error in failed:
        print("Skipped %s: %s" % (filename, error))

    if not saved:
        raise RuntimeError("No diagnostic images were generated")


if __name__ == "__main__":
    main()
