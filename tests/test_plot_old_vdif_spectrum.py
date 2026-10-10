import importlib.util
import pathlib
import struct
import sys
import tempfile
import unittest

import numpy as np


ROOT = pathlib.Path(__file__).parents[1]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))
SCRIPT = SCRIPTS / "plot_old_vdif_spectrum.py"
SPEC = importlib.util.spec_from_file_location("plot_old_vdif_spectrum", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def make_header(payload_bytes, frame_number=0):
    words = [0] * 8
    words[0] = 42
    words[1] = (53 << 24) | frame_number
    # The old recorder could leave the VDIF channel-count field at a bogus
    # value (30 here) even though the payload is one Q,I stream.
    words[2] = (30 << 24) | ((32 + payload_bytes) // 8)
    # Old files contain Q,I complex pairs even though the VDIF complex-data
    # flag (bit 31) was not set.
    words[3] = (7 << 26) | (9 << 16)
    words[4] = 1 << 24
    return struct.pack("<8I", *words)


class OldVdifSpectrumTest(unittest.TestCase):
    def test_offset_binary_qi_order(self):
        samples = MODULE.decode_old_qi_payload(
            bytes((128, 128, 127, 129, 255, 0))
        )
        np.testing.assert_array_equal(samples, [0 + 0j, 1 - 1j, -128 + 127j])

    def test_twos_complement_qi_order(self):
        samples = MODULE.decode_old_qi_payload(
            bytes((0xFF, 0x02, 0x03, 0xFC)), "twos-complement"
        )
        np.testing.assert_array_equal(samples, [2 - 1j, -4 + 3j])

    def test_spectrum_finds_positive_complex_tone(self):
        sample_rate = 256e6
        nfft = 4096
        tone_bin = 211
        index = np.arange(nfft)
        values = 70.0 * np.exp(2j * np.pi * tone_bin * index / nfft)
        q = np.rint(values.imag).clip(-128, 127).astype(np.int16) + 128
        i = np.rint(values.real).clip(-128, 127).astype(np.int16) + 128
        payload = np.column_stack((q, i)).astype(np.uint8).tobytes()

        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "old-tone.vdif"
            with path.open("wb") as stream:
                stream.write(make_header(len(payload)))
                stream.write(payload)
            result = MODULE.calculate_old_spectrum(
                [path], sample_rate_hz=sample_rate, nfft=nfft, max_frames=None
            )

        peak_hz = result.frequency_hz[np.argmax(result.psd_dbfs_hz)]
        self.assertAlmostEqual(peak_hz, tone_bin * sample_rate / nfft)
        self.assertEqual(result.stats.frames_used, 1)
        self.assertEqual(result.bits_per_sample, 8)


if __name__ == "__main__":
    unittest.main()
