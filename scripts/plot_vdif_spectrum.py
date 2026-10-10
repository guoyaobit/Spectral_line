#!/usr/bin/env python3
"""Read complex baseband VDIF files and plot an averaged power spectrum."""

from __future__ import annotations

import argparse
import csv
import dataclasses
import html
import pathlib
import struct
import sys
from collections.abc import Iterator, Sequence

import numpy as np


VDIF_HEADER_BYTES = 32
DEFAULT_SAMPLE_RATE_HZ = 256e6
DEFAULT_NFFT = 65536
DEFAULT_MAX_FRAMES = 2048


@dataclasses.dataclass(frozen=True)
class VdifHeader:
    invalid: bool
    legacy: bool
    seconds_from_epoch: int
    reference_epoch: int
    frame_number: int
    version: int
    log2_channels: int
    frame_bytes: int
    station_id: int
    thread_id: int
    bits_per_sample: int
    complex_data: bool
    edv: int

    def packet_id(self, frames_per_second: int) -> int:
        return self.seconds_from_epoch * frames_per_second + self.frame_number


@dataclasses.dataclass
class ReadStats:
    frames_read: int = 0
    frames_used: int = 0
    invalid_frames: int = 0
    missing_frames: int = 0
    duplicate_frames: int = 0
    reordered_frames: int = 0
    truncated_bytes: int = 0


@dataclasses.dataclass(frozen=True)
class SpectrumResult:
    frequency_hz: np.ndarray
    psd_dbfs_hz: np.ndarray
    averages: int
    bits_per_sample: int
    thread_id: int
    stats: ReadStats


def parse_vdif_header(data: bytes) -> VdifHeader:
    if len(data) != VDIF_HEADER_BYTES:
        raise ValueError(
            f"VDIF header is {len(data)} bytes, expected {VDIF_HEADER_BYTES}"
        )
    words = struct.unpack("<8I", data)
    return VdifHeader(
        invalid=bool((words[0] >> 31) & 1),
        legacy=bool((words[0] >> 30) & 1),
        seconds_from_epoch=words[0] & 0x3FFFFFFF,
        reference_epoch=(words[1] >> 24) & 0x3F,
        frame_number=words[1] & 0x00FFFFFF,
        version=(words[2] >> 29) & 0x7,
        log2_channels=(words[2] >> 24) & 0x1F,
        frame_bytes=(words[2] & 0x00FFFFFF) * 8,
        station_id=words[3] & 0xFFFF,
        thread_id=(words[3] >> 16) & 0x03FF,
        bits_per_sample=((words[3] >> 26) & 0x1F) + 1,
        complex_data=bool((words[3] >> 31) & 1),
        edv=(words[4] >> 24) & 0xFF,
    )


def _signed_fields(values: np.ndarray, bits: int) -> np.ndarray:
    sign = 1 << (bits - 1)
    return ((values.astype(np.int16) ^ sign) - sign).astype(np.float32)


def decode_complex_payload(payload: bytes, bits_per_sample: int) -> np.ndarray:
    """Decode VDIF two's-complement I,Q components into complex64 samples."""
    packed = np.frombuffer(payload, dtype=np.uint8)
    if bits_per_sample == 8:
        if packed.size % 2:
            raise ValueError("8-bit complex payload has an odd byte count")
        components = packed.view(np.int8).astype(np.float32).reshape(-1, 2)
        return (components[:, 0] + 1j * components[:, 1]).astype(np.complex64)

    if bits_per_sample == 4:
        i_values = _signed_fields(packed & 0x0F, 4)
        q_values = _signed_fields(packed >> 4, 4)
        return (i_values + 1j * q_values).astype(np.complex64)

    if bits_per_sample == 2:
        fields = np.empty(packed.size * 4, dtype=np.uint8)
        fields[0::4] = packed & 0x03
        fields[1::4] = (packed >> 2) & 0x03
        fields[2::4] = (packed >> 4) & 0x03
        fields[3::4] = packed >> 6
        components = _signed_fields(fields, 2).reshape(-1, 2)
        return (components[:, 0] + 1j * components[:, 1]).astype(np.complex64)

    raise ValueError(
        f"unsupported VDIF component width {bits_per_sample}; expected 8, 4, or 2"
    )


def iter_vdif_frames(
    paths: Sequence[pathlib.Path],
    stats: ReadStats,
    frames_per_second: int,
    max_frames: int | None,
    require_complex: bool = True,
    require_single_channel: bool = True,
    frame_length_includes_header: bool = True,
) -> Iterator[tuple[VdifHeader, bytes]]:
    expected_format: tuple[int, bool, int] | None = None
    previous_packet_id: int | None = None

    for path in paths:
        with path.open("rb") as stream:
            while max_frames is None or stats.frames_read < max_frames:
                header_data = stream.read(VDIF_HEADER_BYTES)
                if not header_data:
                    break
                if len(header_data) != VDIF_HEADER_BYTES:
                    stats.truncated_bytes += len(header_data)
                    raise ValueError(
                        f"{path}: truncated VDIF header ({len(header_data)} bytes)"
                    )

                header = parse_vdif_header(header_data)
                if header.legacy:
                    raise ValueError(f"{path}: legacy 16-byte VDIF headers are unsupported")
                minimum_frame_bytes = (
                    VDIF_HEADER_BYTES if frame_length_includes_header else 1
                )
                if header.frame_bytes < minimum_frame_bytes:
                    raise ValueError(
                        f"{path}: invalid VDIF frame length {header.frame_bytes}"
                    )
                if require_complex and not header.complex_data:
                    raise ValueError(f"{path}: VDIF frame is not marked as complex data")
                if require_single_channel and header.log2_channels != 0:
                    raise ValueError(
                        f"{path}: expected one channel, header declares "
                        f"2^{header.log2_channels} channels"
                    )

                # Standard VDIF stores the complete frame length.  Files from
                # the receiver before c5eb8de stored only the payload length in
                # this field; the old-format reader selects that convention.
                payload_bytes = header.frame_bytes
                if frame_length_includes_header:
                    payload_bytes -= VDIF_HEADER_BYTES
                payload = stream.read(payload_bytes)
                stats.frames_read += 1
                if len(payload) != payload_bytes:
                    stats.truncated_bytes += len(payload)
                    raise ValueError(
                        f"{path}: truncated frame {header.frame_number}: read "
                        f"{len(payload)} of {payload_bytes} payload bytes"
                    )

                frame_format = (
                    header.bits_per_sample,
                    header.complex_data,
                    header.thread_id,
                )
                if expected_format is None:
                    expected_format = frame_format
                elif frame_format != expected_format:
                    raise ValueError(
                        f"{path}: VDIF format/thread changed from "
                        f"{expected_format} to {frame_format}"
                    )

                packet_id = header.packet_id(frames_per_second)
                if previous_packet_id is not None:
                    difference = packet_id - previous_packet_id
                    if difference > 1:
                        stats.missing_frames += difference - 1
                    elif difference == 0:
                        stats.duplicate_frames += 1
                    elif difference < 0:
                        stats.reordered_frames += 1
                previous_packet_id = packet_id

                if header.invalid:
                    stats.invalid_frames += 1
                    continue
                stats.frames_used += 1
                yield header, payload

        if max_frames is not None and stats.frames_read >= max_frames:
            break


def _window(name: str, nfft: int) -> np.ndarray:
    if name == "hann":
        return np.hanning(nfft).astype(np.float32)
    if name == "rectangular":
        return np.ones(nfft, dtype=np.float32)
    raise ValueError(f"unknown window {name}")


def calculate_spectrum(
    paths: Sequence[pathlib.Path],
    sample_rate_hz: float = DEFAULT_SAMPLE_RATE_HZ,
    nfft: int = DEFAULT_NFFT,
    max_frames: int | None = DEFAULT_MAX_FRAMES,
    frames_per_second: int = 62500,
    window_name: str = "hann",
) -> SpectrumResult:
    if not paths:
        raise ValueError("at least one input VDIF file is required")
    if sample_rate_hz <= 0:
        raise ValueError("sample rate must be positive")
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
    first_header: VdifHeader | None = None

    frames = iter_vdif_frames(paths, stats, frames_per_second, max_frames)
    for header, payload in frames:
        if first_header is None:
            first_header = header
        samples = decode_complex_payload(payload, header.bits_per_sample)
        full_scale = float(1 << (header.bits_per_sample - 1))
        samples /= full_scale
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
    psd = np.fft.fftshift(psd)
    frequency_hz = np.fft.fftshift(np.fft.fftfreq(nfft, 1.0 / sample_rate_hz))
    floor = np.finfo(np.float64).tiny
    psd_dbfs_hz = 10.0 * np.log10(np.maximum(psd, floor))
    return SpectrumResult(
        frequency_hz=frequency_hz,
        psd_dbfs_hz=psd_dbfs_hz,
        averages=averages,
        bits_per_sample=first_header.bits_per_sample,
        thread_id=first_header.thread_id,
        stats=stats,
    )


def frequency_axis(
    baseband_hz: np.ndarray,
    center_frequency_hz: float | None,
    sideband: str,
) -> tuple[np.ndarray, str]:
    if center_frequency_hz is None:
        return baseband_hz / 1e6, "Baseband frequency offset (MHz)"
    sign = 1.0 if sideband == "upper" else -1.0
    return (center_frequency_hz + sign * baseband_hz) / 1e6, "Frequency (MHz)"


def plot_spectrum(
    result: SpectrumResult,
    output: pathlib.Path,
    center_frequency_hz: float | None,
    sideband: str,
    title: str | None,
) -> None:
    if output.suffix.lower() == ".html":
        write_interactive_html(
            result, output, center_frequency_hz, sideband, title
        )
        return

    try:
        import matplotlib
    except ModuleNotFoundError as error:
        raise ValueError(
            "matplotlib is required for plotting; install the dependencies "
            "from monitor/requirements.txt"
        ) from error

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    x, xlabel = frequency_axis(
        result.frequency_hz, center_frequency_hz, sideband
    )
    order = np.argsort(x)
    figure, axis = plt.subplots(figsize=(12, 6), constrained_layout=True)
    axis.plot(x[order], result.psd_dbfs_hz[order], linewidth=0.75)
    axis.set_xlabel(xlabel)
    axis.set_ylabel("Power spectral density (dBFS/Hz)")
    axis.set_title(title or "VDIF complex baseband spectrum")
    axis.grid(True, alpha=0.25)
    axis.text(
        0.01,
        0.02,
        f"{result.bits_per_sample}-bit  thread {result.thread_id}  "
        f"{result.averages} FFT averages",
        transform=axis.transAxes,
        fontsize=9,
        verticalalignment="bottom",
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(output, dpi=180)
    plt.close(figure)


def _javascript_number_array(values: np.ndarray) -> str:
    return ",".join(f"{float(value):.9g}" for value in values)


def write_interactive_html(
    result: SpectrumResult,
    output: pathlib.Path,
    center_frequency_hz: float | None,
    sideband: str,
    title: str | None,
) -> None:
    frequency_mhz, xlabel = frequency_axis(
        result.frequency_hz, center_frequency_hz, sideband
    )
    order = np.argsort(frequency_mhz)
    x = np.asarray(frequency_mhz[order], dtype=np.float64)
    y = np.asarray(result.psd_dbfs_hz[order], dtype=np.float64)
    finite = np.isfinite(x) & np.isfinite(y)
    x = x[finite]
    y = y[finite]
    if x.size < 2:
        raise ValueError("interactive spectrum needs at least two finite points")

    visible_title = html.escape(title or "VDIF complex baseband spectrum")
    visible_xlabel = html.escape(xlabel)
    metadata = html.escape(
        f"{result.bits_per_sample}-bit · thread {result.thread_id} · "
        f"{result.averages} FFT averages"
    )
    template = r"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<style>
  :root { color-scheme: light dark; font-family: system-ui, sans-serif; }
  body { margin: 0; padding: 20px; background: Canvas; color: CanvasText; }
  .toolbar { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; margin-bottom: 8px; }
  h1 { margin: 0; font-size: 20px; font-weight: 500; }
  .meta, .hint { color: GrayText; font-size: 13px; }
  button { font: inherit; padding: 5px 10px; }
  .chart { position: relative; width: 100%; }
  canvas { display: block; width: 100%; height: min(72vh, 720px); min-height: 420px; touch-action: none; }
  .tooltip { position: absolute; display: none; pointer-events: none; padding: 6px 8px;
    border: 1px solid GrayText; border-radius: 4px; background: Canvas; color: CanvasText;
    font-size: 12px; white-space: nowrap; box-shadow: 0 2px 8px rgb(0 0 0 / 20%); }
</style>
</head>
<body>
<div class="toolbar">
  <h1>__TITLE__</h1>
  <span class="meta">__META__</span>
  <button id="reset" type="button">Reset view</button>
</div>
<div class="hint">Mouse wheel: zoom frequency · drag: pan · hover: inspect</div>
<div class="chart" id="chart">
  <canvas id="spectrum" aria-label="Interactive VDIF power spectrum"></canvas>
  <div class="tooltip" id="tooltip" role="status"></div>
</div>
<script>
(() => {
  "use strict";
  const xs = [__X_DATA__];
  const ys = [__Y_DATA__];
  const xLabel = "__X_LABEL__";
  const yLabel = "Power spectral density (dBFS/Hz)";
  const canvas = document.getElementById("spectrum");
  const tooltip = document.getElementById("tooltip");
  const ctx = canvas.getContext("2d");
  const fullMin = xs[0], fullMax = xs[xs.length - 1];
  let viewMin = fullMin, viewMax = fullMax;
  let dragging = false, dragStartX = 0, dragMin = 0, dragMax = 0;
  let width = 0, height = 0, pendingDraw = false;
  const margin = { left: 82, right: 24, top: 32, bottom: 66 };
  let yMin = Infinity, yMax = -Infinity;
  for (const value of ys) { yMin = Math.min(yMin, value); yMax = Math.max(yMax, value); }
  const yPad = Math.max((yMax - yMin) * 0.05, 1);
  yMin -= yPad; yMax += yPad;

  function colors() {
    return matchMedia("(prefers-color-scheme: dark)").matches
      ? { fg: "#e8e8e8", muted: "#a8a8a8", grid: "#444", line: "#6bb6ff", bg: "#121212" }
      : { fg: "#202020", muted: "#666", grid: "#d8d8d8", line: "#1769aa", bg: "#fff" };
  }
  function plotWidth() { return width - margin.left - margin.right; }
  function plotHeight() { return height - margin.top - margin.bottom; }
  function xToPixel(value) { return margin.left + (value - viewMin) / (viewMax - viewMin) * plotWidth(); }
  function pixelToX(value) { return viewMin + (value - margin.left) / plotWidth() * (viewMax - viewMin); }
  function yToPixel(value) { return margin.top + (yMax - value) / (yMax - yMin) * plotHeight(); }
  function lowerBound(value) {
    let lo = 0, hi = xs.length;
    while (lo < hi) { const mid = (lo + hi) >> 1; if (xs[mid] < value) lo = mid + 1; else hi = mid; }
    return lo;
  }
  function clampView() {
    const span = viewMax - viewMin;
    if (span >= fullMax - fullMin) { viewMin = fullMin; viewMax = fullMax; return; }
    if (viewMin < fullMin) { viewMin = fullMin; viewMax = fullMin + span; }
    if (viewMax > fullMax) { viewMax = fullMax; viewMin = fullMax - span; }
  }
  function tickText(value, span) {
    if (span < 0.01) return value.toFixed(6);
    if (span < 1) return value.toFixed(4);
    return value.toFixed(2);
  }
  function drawAxes(c) {
    ctx.strokeStyle = c.grid; ctx.fillStyle = c.muted; ctx.lineWidth = 1; ctx.font = "12px system-ui";
    ctx.textAlign = "center"; ctx.textBaseline = "top";
    const xTicks = Math.max(3, Math.min(8, Math.floor(plotWidth() / 100)));
    for (let n = 0; n <= xTicks; n++) {
      const px = margin.left + n / xTicks * plotWidth();
      const value = viewMin + n / xTicks * (viewMax - viewMin);
      ctx.beginPath(); ctx.moveTo(px, margin.top); ctx.lineTo(px, height - margin.bottom); ctx.stroke();
      ctx.fillText(tickText(value, viewMax - viewMin), px, height - margin.bottom + 8);
    }
    ctx.textAlign = "right"; ctx.textBaseline = "middle";
    for (let n = 0; n <= 6; n++) {
      const py = margin.top + n / 6 * plotHeight();
      const value = yMax - n / 6 * (yMax - yMin);
      ctx.beginPath(); ctx.moveTo(margin.left, py); ctx.lineTo(width - margin.right, py); ctx.stroke();
      ctx.fillText(value.toFixed(1), margin.left - 8, py);
    }
    ctx.fillStyle = c.fg; ctx.textAlign = "center"; ctx.textBaseline = "bottom";
    ctx.fillText(xLabel, margin.left + plotWidth() / 2, height - 4);
    ctx.save(); ctx.translate(16, margin.top + plotHeight() / 2); ctx.rotate(-Math.PI / 2);
    ctx.fillText(yLabel, 0, 0); ctx.restore();
    ctx.strokeStyle = c.fg; ctx.strokeRect(margin.left, margin.top, plotWidth(), plotHeight());
  }
  function drawSeries(c) {
    const start = Math.max(0, lowerBound(viewMin) - 1);
    const end = Math.min(xs.length, lowerBound(viewMax) + 1);
    ctx.save(); ctx.beginPath(); ctx.rect(margin.left, margin.top, plotWidth(), plotHeight()); ctx.clip();
    ctx.strokeStyle = c.line; ctx.lineWidth = 1; ctx.beginPath();
    if (end - start <= plotWidth() * 2) {
      for (let i = start; i < end; i++) {
        const px = xToPixel(xs[i]), py = yToPixel(ys[i]);
        if (i === start) ctx.moveTo(px, py); else ctx.lineTo(px, py);
      }
    } else {
      for (let pixel = 0; pixel < plotWidth(); pixel++) {
        const xa = pixelToX(margin.left + pixel), xb = pixelToX(margin.left + pixel + 1);
        const i0 = Math.max(start, lowerBound(xa));
        const i1 = Math.min(end, Math.max(i0 + 1, lowerBound(xb)));
        let lo = Infinity, hi = -Infinity;
        for (let i = i0; i < i1; i++) { lo = Math.min(lo, ys[i]); hi = Math.max(hi, ys[i]); }
        if (Number.isFinite(lo)) { const px = margin.left + pixel + 0.5; ctx.moveTo(px, yToPixel(lo)); ctx.lineTo(px, yToPixel(hi)); }
      }
    }
    ctx.stroke(); ctx.restore();
  }
  function draw() {
    pendingDraw = false; const c = colors();
    ctx.clearRect(0, 0, width, height); ctx.fillStyle = c.bg; ctx.fillRect(0, 0, width, height);
    drawAxes(c); drawSeries(c);
  }
  function queueDraw() { if (!pendingDraw) { pendingDraw = true; requestAnimationFrame(draw); } }
  function resize() {
    const rect = canvas.getBoundingClientRect(), dpr = window.devicePixelRatio || 1;
    width = Math.max(320, rect.width); height = Math.max(420, rect.height);
    canvas.width = Math.round(width * dpr); canvas.height = Math.round(height * dpr);
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0); queueDraw();
  }
  function pointerPosition(event) { const r = canvas.getBoundingClientRect(); return { x: event.clientX - r.left, y: event.clientY - r.top }; }
  function hideTooltip() { tooltip.style.display = "none"; }
  function showHover(event) {
    const p = pointerPosition(event);
    if (p.x < margin.left || p.x > width - margin.right || p.y < margin.top || p.y > height - margin.bottom) { hideTooltip(); return; }
    const target = pixelToX(p.x); let index = lowerBound(target);
    if (index >= xs.length) index = xs.length - 1;
    if (index > 0 && Math.abs(xs[index - 1] - target) < Math.abs(xs[index] - target)) index--;
    draw(); const c = colors(), px = xToPixel(xs[index]), py = yToPixel(ys[index]);
    ctx.strokeStyle = c.muted; ctx.beginPath(); ctx.moveTo(px, margin.top); ctx.lineTo(px, height - margin.bottom); ctx.stroke();
    ctx.fillStyle = c.line; ctx.beginPath(); ctx.arc(px, py, 3, 0, Math.PI * 2); ctx.fill();
    tooltip.textContent = `Frequency: ${xs[index].toFixed(6)} MHz · PSD: ${ys[index].toFixed(2)} dBFS/Hz`;
    tooltip.style.display = "block";
    const tw = tooltip.offsetWidth, th = tooltip.offsetHeight;
    tooltip.style.left = `${Math.min(width - tw - 4, Math.max(4, p.x + 12))}px`;
    tooltip.style.top = `${Math.min(height - th - 4, Math.max(4, p.y - th - 10))}px`;
  }
  canvas.addEventListener("wheel", event => {
    if (event.ctrlKey) return;
    event.preventDefault(); hideTooltip();
    const p = pointerPosition(event), center = pixelToX(Math.min(width - margin.right, Math.max(margin.left, p.x)));
    const oldSpan = viewMax - viewMin, fullSpan = fullMax - fullMin;
    const newSpan = Math.min(fullSpan, Math.max(fullSpan / 1e6, oldSpan * Math.exp(event.deltaY * 0.0015)));
    const ratio = (center - viewMin) / oldSpan;
    viewMin = center - ratio * newSpan; viewMax = viewMin + newSpan; clampView(); queueDraw();
  }, { passive: false });
  canvas.addEventListener("pointerdown", event => {
    const p = pointerPosition(event); dragging = true; dragStartX = p.x; dragMin = viewMin; dragMax = viewMax;
    canvas.setPointerCapture(event.pointerId); hideTooltip();
  });
  canvas.addEventListener("pointermove", event => {
    if (!dragging) { showHover(event); return; }
    const p = pointerPosition(event), shift = -(p.x - dragStartX) / plotWidth() * (dragMax - dragMin);
    viewMin = dragMin + shift; viewMax = dragMax + shift; clampView(); queueDraw();
  });
  function stopDrag(event) { if (dragging) { dragging = false; canvas.releasePointerCapture(event.pointerId); } }
  canvas.addEventListener("pointerup", stopDrag); canvas.addEventListener("pointercancel", stopDrag);
  canvas.addEventListener("pointerleave", () => { if (!dragging) hideTooltip(); });
  canvas.addEventListener("dblclick", () => { viewMin = fullMin; viewMax = fullMax; hideTooltip(); queueDraw(); });
  document.getElementById("reset").addEventListener("click", () => { viewMin = fullMin; viewMax = fullMax; hideTooltip(); queueDraw(); });
  new ResizeObserver(resize).observe(canvas);
  matchMedia("(prefers-color-scheme: dark)").addEventListener("change", queueDraw);
  resize();
})();
</script>
</body>
</html>
"""
    document = (
        template.replace("__TITLE__", visible_title)
        .replace("__META__", metadata)
        .replace("__X_LABEL__", visible_xlabel)
        .replace("__X_DATA__", _javascript_number_array(x))
        .replace("__Y_DATA__", _javascript_number_array(y))
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(document, encoding="utf-8")


def write_csv(
    path: pathlib.Path,
    result: SpectrumResult,
    center_frequency_hz: float | None,
    sideband: str,
) -> None:
    frequency_mhz, _ = frequency_axis(
        result.frequency_hz, center_frequency_hz, sideband
    )
    order = np.argsort(frequency_mhz)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("frequency_mhz", "psd_dbfs_hz"))
        writer.writerows(zip(frequency_mhz[order], result.psd_dbfs_hz[order]))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Decode this receiver's 8/4/2-bit complex VDIF baseband files, "
            "average FFT power spectra, and write an interactive HTML plot."
        )
    )
    parser.add_argument("input", nargs="+", type=pathlib.Path)
    parser.add_argument(
        "-o",
        "--output",
        type=pathlib.Path,
        help="output plot (default: interactive .html; .svg/.pdf/.png are static)",
    )
    parser.add_argument("--csv", type=pathlib.Path, help="also write spectrum CSV")
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
            "use 0 for the complete input)"
        ),
    )
    parser.add_argument("--frames-per-second", type=int, default=62500)
    parser.add_argument(
        "--window", choices=("hann", "rectangular"), default="hann"
    )
    parser.add_argument(
        "--center-frequency-hz",
        type=float,
        help="convert the x axis from baseband offset to sky/RF frequency",
    )
    parser.add_argument(
        "--sideband",
        choices=("upper", "lower"),
        default="upper",
        help="frequency orientation when --center-frequency-hz is set",
    )
    parser.add_argument("--title")
    return parser


def _format_summary(result: SpectrumResult) -> str:
    stats = result.stats
    peak = int(np.argmax(result.psd_dbfs_hz))
    return (
        f"frames read={stats.frames_read}, used={stats.frames_used}, "
        f"invalid={stats.invalid_frames}, missing={stats.missing_frames}, "
        f"duplicate={stats.duplicate_frames}, reordered={stats.reordered_frames}; "
        f"FFT averages={result.averages}, peak offset="
        f"{result.frequency_hz[peak] / 1e6:.6f} MHz, "
        f"peak PSD={result.psd_dbfs_hz[peak]:.2f} dBFS/Hz"
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    missing = [str(path) for path in args.input if not path.is_file()]
    if missing:
        parser.error("input file not found: " + ", ".join(missing))
    if args.frames_per_second <= 0:
        parser.error("--frames-per-second must be positive")

    output = args.output or args.input[0].with_suffix(".spectrum.html")
    max_frames = None if args.max_frames == 0 else args.max_frames
    try:
        result = calculate_spectrum(
            args.input,
            sample_rate_hz=args.sample_rate_hz,
            nfft=args.nfft,
            max_frames=max_frames,
            frames_per_second=args.frames_per_second,
            window_name=args.window,
        )
        plot_spectrum(
            result,
            output,
            args.center_frequency_hz,
            args.sideband,
            args.title,
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
    print(f"wrote {output}")
    if args.csv:
        print(f"wrote {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
