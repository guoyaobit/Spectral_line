#!/usr/bin/env python3
"""Continuously render diagnostics for all /dev/shm monitor streams."""

import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import time

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
from PIL import Image


MONITOR_DIR = Path("/dev/shm")
OUTPUT_DIR = MONITOR_DIR / "monitor_plots"
STREAM_PATTERN = "server_*_stream_*.bin"
HEADER_SIZE = 32
PAYLOAD_SIZE = 8192
SAMPLE_RATE = 256e6
ADC_PLOT_SAMPLES = 1024
POLL_INTERVAL_SECONDS = 1.0
HIGH_SIZE = (12, 4.5)
HIGH_DPI = 300
THUMBNAIL_PIXELS = (320, 120)
MAX_STREAMS = 16
DEFAULT_WORKERS = min(MAX_STREAMS, os.cpu_count() or 1)
IMAGE_KINDS = (
    ("adc", "adc_raw"),
    ("fft", "fft_spectrum"),
    ("histogram", "adc_histogram"),
)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Continuously render all /dev/shm spectral monitor streams."
    )
    parser.add_argument(
        "--once",
        action="store_true",
        help="render the current stream snapshots once and exit",
    )
    return parser.parse_args()


def configured_workers():
    raw_value = os.environ.get("MONITOR_PLOT_WORKERS", str(DEFAULT_WORKERS))
    try:
        workers = int(raw_value)
    except ValueError as error:
        raise ValueError("MONITOR_PLOT_WORKERS must be an integer") from error
    if workers <= 0:
        raise ValueError("MONITOR_PLOT_WORKERS must be greater than zero")
    return min(workers, MAX_STREAMS)


def read_monitor_snapshot(filename, retries=5, retry_delay=0.01):
    expected_size = HEADER_SIZE + PAYLOAD_SIZE
    if filename.stat().st_size < expected_size:
        raise RuntimeError(
            "file is too short: expected at least %d bytes" % expected_size
        )

    snapshot = None
    for _attempt in range(retries):
        first = filename.read_bytes()[:expected_size]
        time.sleep(retry_delay)
        second = filename.read_bytes()[:expected_size]
        if len(first) == expected_size and first == second:
            snapshot = second
            break

    if snapshot is None:
        raise RuntimeError("frame changed while being read")
    if not any(snapshot[:HEADER_SIZE]):
        raise RuntimeError("stream has not received its first frame")

    digest = hashlib.blake2b(snapshot, digest_size=16).digest()
    return snapshot, digest


def decode_adc(snapshot):
    payload = snapshot[HEADER_SIZE:HEADER_SIZE + PAYLOAD_SIZE]
    adc = np.frombuffer(payload, dtype=np.int8)
    return adc[1::2], adc[0::2]


def temporary_path(destination):
    return destination.with_name(
        ".%s.%d.tmp" % (destination.name, os.getpid())
    )


def save_versions(figure, output_dir, name):
    high_dir = output_dir / "high_resolution"
    thumbnail_dir = output_dir / "thumbnails"
    high_dir.mkdir(parents=True, exist_ok=True)
    thumbnail_dir.mkdir(parents=True, exist_ok=True)

    high_path = high_dir / (name + ".png")
    thumbnail_path = thumbnail_dir / (name + ".png")
    high_temporary = temporary_path(high_path)
    thumbnail_temporary = temporary_path(thumbnail_path)

    try:
        figure.savefig(
            high_temporary,
            format="png",
            dpi=HIGH_DPI,
            bbox_inches="tight",
            facecolor="white",
        )
        with Image.open(high_temporary) as image:
            image.thumbnail(THUMBNAIL_PIXELS, Image.Resampling.LANCZOS)
            image.save(thumbnail_temporary, format="PNG", optimize=True)

        # Each file is always complete when it becomes visible to readers.
        high_temporary.replace(high_path)
        thumbnail_temporary.replace(thumbnail_path)
    finally:
        high_temporary.unlink(missing_ok=True)
        thumbnail_temporary.unlink(missing_ok=True)

    return high_path, thumbnail_path


def render_adc(real, imag, output_dir, prefix):
    count = min(ADC_PLOT_SAMPLES, real.size)
    sample_index = np.arange(count)
    time_us = sample_index / SAMPLE_RATE * 1e6
    figure, axes = plt.subplots(figsize=HIGH_SIZE)
    try:
        axes.plot(time_us, real[:count], linewidth=0.8, label="Real")
        axes.plot(time_us, imag[:count], linewidth=0.8, alpha=0.8, label="Imag")
        axes.set_title("Raw ADC Samples")
        axes.set_xlabel("Time (us)")
        axes.set_ylabel("ADC value")
        axes.set_ylim(-132, 132)
        axes.grid(alpha=0.3)
        axes.legend(loc="upper right", ncol=2)
        figure.tight_layout()
        return save_versions(figure, output_dir, prefix + "_adc_raw")
    finally:
        plt.close(figure)


def render_fft(real, imag, output_dir, prefix):
    signal = real.astype(np.float32) + 1j * imag.astype(np.float32)
    spectrum = np.fft.fftshift(np.fft.fft(signal))
    power = np.abs(spectrum) ** 2
    reference = max(float(np.max(power)), np.finfo(np.float32).tiny)
    power_db = 10.0 * np.log10(power / reference + 1e-12)
    frequency_mhz = (
        np.fft.fftshift(np.fft.fftfreq(signal.size, d=1.0 / SAMPLE_RATE)) / 1e6
    )

    figure, axes = plt.subplots(figsize=HIGH_SIZE)
    try:
        axes.plot(frequency_mhz, power_db, linewidth=0.8)
        axes.set_title("FFT Spectrum")
        axes.set_xlabel("Frequency (MHz)")
        axes.set_ylabel("Relative power (dB)")
        axes.set_xlim(frequency_mhz[0], frequency_mhz[-1])
        axes.grid(alpha=0.3)
        figure.tight_layout()
        return save_versions(figure, output_dir, prefix + "_fft_spectrum")
    finally:
        plt.close(figure)


def render_histogram(real, imag, output_dir, prefix):
    bins = np.arange(-128.5, 128.6, 2.0)
    figure, axes = plt.subplots(figsize=HIGH_SIZE)
    try:
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
        axes.legend(loc="upper right")
        figure.tight_layout()
        return save_versions(figure, output_dir, prefix + "_adc_histogram")
    finally:
        plt.close(figure)


def render_stream(prefix, snapshot, output_dir):
    real, imag = decode_adc(snapshot)
    saved = []
    saved.extend(render_adc(real, imag, output_dir, prefix))
    saved.extend(render_fft(real, imag, output_dir, prefix))
    saved.extend(render_histogram(real, imag, output_dir, prefix))
    return saved


def manifest_entry(filename, digest):
    images = {}
    for kind, suffix in IMAGE_KINDS:
        image_name = "%s_%s.png" % (filename.stem, suffix)
        images[kind] = {
            "thumbnail": "thumbnails/" + image_name,
            "high_resolution": "high_resolution/" + image_name,
        }
    return {
        "name": filename.stem,
        "source": filename.name,
        "revision": digest.hex(),
        "updated_at": datetime.now(timezone.utc).isoformat(),
        "images": images,
    }


def publish_manifest(entries):
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    manifest = {
        "version": str(time.time_ns()),
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "poll_interval_ms": int(POLL_INTERVAL_SECONDS * 1000),
        "streams": [entries[name] for name in sorted(entries)],
    }
    destination = OUTPUT_DIR / "manifest.json"
    temporary = temporary_path(destination)
    try:
        temporary.write_text(
            json.dumps(manifest, ensure_ascii=False, separators=(",", ":")),
            encoding="utf-8",
        )
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)


def collect_updates(previous_digests, reported_errors):
    updates = []
    stream_files = sorted(
        path for path in MONITOR_DIR.glob(STREAM_PATTERN) if path.is_file()
    )
    for filename in stream_files:
        try:
            snapshot, digest = read_monitor_snapshot(filename)
            if previous_digests.get(filename) != digest:
                updates.append((filename, snapshot, digest))
            reported_errors.pop(filename, None)
        except (OSError, RuntimeError) as error:
            message = str(error)
            if reported_errors.get(filename) != message:
                print("Skipped %s: %s" % (filename, message), flush=True)
                reported_errors[filename] = message
    return stream_files, updates


def run_monitor(render_once):
    digests = {}
    manifest_entries = {}
    reported_errors = {}
    workers = configured_workers()
    print(
        "Monitoring %s with %d rendering process(es)." % (MONITOR_DIR, workers),
        flush=True,
    )

    with ProcessPoolExecutor(max_workers=workers) as executor:
        while True:
            cycle_started = time.monotonic()
            stream_files, updates = collect_updates(digests, reported_errors)
            active_files = set(stream_files)
            removed = [path for path in digests if path not in active_files]
            for filename in removed:
                digests.pop(filename, None)
                manifest_entries.pop(filename.stem, None)

            futures = {
                executor.submit(
                    render_stream, filename.stem, snapshot, OUTPUT_DIR
                ): (filename, digest)
                for filename, snapshot, digest in updates
            }

            rendered = 0
            cycle_succeeded = True
            completed = []
            for future in as_completed(futures):
                filename, digest = futures[future]
                try:
                    saved = future.result()
                    completed.append((filename, digest))
                    rendered += 1
                    print(
                        "Updated %s (%d images)." % (filename.name, len(saved)),
                        flush=True,
                    )
                except Exception as error:
                    cycle_succeeded = False
                    print("Failed %s: %s" % (filename, error), flush=True)

            if cycle_succeeded:
                for filename, digest in completed:
                    digests[filename] = digest
                    manifest_entries[filename.stem] = manifest_entry(
                        filename, digest
                    )

            if cycle_succeeded and (
                rendered or removed or not (OUTPUT_DIR / "manifest.json").exists()
            ):
                publish_manifest(manifest_entries)

            if render_once:
                if not stream_files:
                    raise RuntimeError(
                        "No monitor files matching %s in %s"
                        % (STREAM_PATTERN, MONITOR_DIR)
                    )
                if not cycle_succeeded or rendered == 0:
                    raise RuntimeError("One or more diagnostic renders failed")
                return

            elapsed = time.monotonic() - cycle_started
            time.sleep(max(0.0, POLL_INTERVAL_SECONDS - elapsed))


def main():
    args = parse_args()
    try:
        run_monitor(args.once)
    except KeyboardInterrupt:
        print("Monitoring stopped.", flush=True)


if __name__ == "__main__":
    main()
