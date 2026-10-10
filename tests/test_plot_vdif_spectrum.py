import importlib.util
import pathlib
import struct
import sys
import tempfile
import unittest

import numpy as np


SCRIPT = pathlib.Path(__file__).parents[1] / "scripts" / "plot_vdif_spectrum.py"
SPEC = importlib.util.spec_from_file_location("plot_vdif_spectrum", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def make_header(bits, payload_bytes, frame_number=0, thread_id=7):
    words = [0] * 8
    words[0] = 42
    words[1] = (53 << 24) | frame_number
    words[2] = (1 << 29) | ((32 + payload_bytes) // 8)
    words[3] = (1 << 31) | ((bits - 1) << 26) | (thread_id << 16)
    words[4] = 1 << 24
    return struct.pack("<8I", *words)


class VdifSpectrumTest(unittest.TestCase):
    def test_decode_known_packed_values(self):
        eight = MODULE.decode_complex_payload(bytes((0xFF, 0x80, 0x7F, 0x00)), 8)
        np.testing.assert_array_equal(eight, [-1 - 128j, 127 + 0j])

        four = MODULE.decode_complex_payload(bytes((0x89, 0x27)), 4)
        np.testing.assert_array_equal(four, [-7 - 8j, 7 + 2j])

        two = MODULE.decode_complex_payload(bytes((0x1B,)), 2)
        np.testing.assert_array_equal(two, [-1 - 2j, 1 + 0j])

    def test_header_fields(self):
        header = MODULE.parse_vdif_header(make_header(4, 4096, 123, 31))
        self.assertEqual(header.bits_per_sample, 4)
        self.assertEqual(header.frame_number, 123)
        self.assertEqual(header.thread_id, 31)
        self.assertEqual(header.frame_bytes, 4128)
        self.assertTrue(header.complex_data)

    def test_standard_word0_flag_positions(self):
        invalid_words = list(struct.unpack("<8I", make_header(8, 8192)))
        invalid_words[0] = (1 << 31) | 42
        invalid = MODULE.parse_vdif_header(struct.pack("<8I", *invalid_words))
        self.assertTrue(invalid.invalid)
        self.assertFalse(invalid.legacy)

        legacy_words = invalid_words.copy()
        legacy_words[0] = (1 << 30) | 42
        legacy = MODULE.parse_vdif_header(struct.pack("<8I", *legacy_words))
        self.assertFalse(legacy.invalid)
        self.assertTrue(legacy.legacy)

    def test_spectrum_finds_complex_tone(self):
        sample_rate = 256e6
        nfft = 4096
        tone_bin = 321
        index = np.arange(nfft)
        samples = 80.0 * np.exp(2j * np.pi * tone_bin * index / nfft)
        i_values = np.rint(samples.real).clip(-128, 127).astype(np.int8)
        q_values = np.rint(samples.imag).clip(-128, 127).astype(np.int8)
        payload = np.column_stack((i_values, q_values)).tobytes()

        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "tone.vdif"
            plot_path = pathlib.Path(directory) / "tone.png"
            with path.open("wb") as stream:
                stream.write(make_header(8, len(payload)))
                stream.write(payload)
            result = MODULE.calculate_spectrum(
                [path], sample_rate_hz=sample_rate, nfft=nfft, max_frames=None
            )
            if importlib.util.find_spec("matplotlib") is not None:
                MODULE.plot_spectrum(result, plot_path, None, "upper", None)
                self.assertGreater(plot_path.stat().st_size, 1000)

        peak_hz = result.frequency_hz[np.argmax(result.psd_dbfs_hz)]
        expected_hz = tone_bin * sample_rate / nfft
        self.assertAlmostEqual(peak_hz, expected_hz)
        self.assertEqual(result.averages, 1)
        self.assertEqual(result.stats.frames_used, 1)


if __name__ == "__main__":
    unittest.main()
