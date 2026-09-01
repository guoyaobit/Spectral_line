#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import sys
import numpy as np
import matplotlib.pyplot as plt

HEADER_SIZE = 32
PAYLOAD_SIZE = 8192
SAMPLE_RATE = 256e6


def main(filename):
    with open(filename, "rb") as f:
        f.seek(HEADER_SIZE)
        data = np.frombuffer(f.read(PAYLOAD_SIZE), dtype=np.int8)

    if len(data) != PAYLOAD_SIZE:
        raise RuntimeError("Invalid payload size")

    # Payload format:
    # Imag0, Real0, Imag1, Real1, ...
    imag = data[0::2].astype(np.float32)
    real = data[1::2].astype(np.float32)

    # Complex baseband signal
    signal = real + 1j * imag

    # ==========================================
    # 1. Time domain power
    # ==========================================
    power_time = np.abs(signal) ** 2

    nplot = min(1024, len(signal))
    t = np.arange(nplot) / SAMPLE_RATE * 1e6

    plt.figure(figsize=(12, 4))
    # plt.plot(t, power_time[:nplot])
    plt.plot(power_time)
    plt.xlabel("Time (us)")
    plt.ylabel("Power")
    plt.title("Time Domain Power")
    plt.grid()
    plt.tight_layout()

    # ==========================================
    # 2. Frequency domain power
    # ==========================================
    nfft = len(signal)

    spectrum = np.fft.fftshift(np.fft.fft(signal))
    power_freq = np.abs(spectrum) ** 2

    # Normalize to dB
    power_freq_db = 10 * np.log10(
        power_freq / np.max(power_freq) + 1e-12
    )

    freq = np.fft.fftshift(
        np.fft.fftfreq(nfft, d=1.0 / SAMPLE_RATE)
    ) / 1e6

    plt.figure(figsize=(12, 4))
    plt.plot(freq,power_freq_db)
    plt.xlabel("Frequency (MHz)")
    plt.ylabel("Power (dB)")
    plt.title("Frequency Domain Power")
    plt.grid()
    plt.tight_layout()

    # ==========================================
    # 3. Gaussian distribution
    # ==========================================
    amplitude = np.concatenate([real, imag])

    mu = np.mean(amplitude)
    sigma = np.std(amplitude)

    plt.figure(figsize=(10, 4))

    counts, bins, _ = plt.hist(
        amplitude,
        bins=128,
        density=True,
        alpha=0.7,
        label="Data"
    )

    x = np.linspace(bins[0], bins[-1], 1000)

    gaussian = (
        1.0 / (sigma * np.sqrt(2 * np.pi))
        * np.exp(-0.5 * ((x - mu) / sigma) ** 2)
    )

    plt.plot(
        x,
        gaussian,
        linewidth=2,
        label="Gaussian"
    )

    plt.xlabel("Amplitude")
    plt.ylabel("Probability Density")
    plt.title(
        "Amplitude Distribution: "
        "mean=%.2f, std=%.2f" % (mu, sigma)
    )
    plt.grid()
    plt.legend()
    plt.tight_layout()

    plt.show()


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: %s <vdif_file>" % sys.argv[0])
        sys.exit(1)

    main(sys.argv[1])