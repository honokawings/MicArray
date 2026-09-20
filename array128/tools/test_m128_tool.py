"""Quick independent checks for critical fixture and simulator layout."""
import hashlib
import unittest

import m128_tool as protocol


class ProtocolTests(unittest.TestCase):
    def test_real_rtl_fixture(self):
        data = (protocol.ROOT / "tests/fixtures/selftest_packet_v1.bin").read_bytes()
        self.assertEqual(hashlib.sha256(data).hexdigest(),
                         "0ee3bf2bd10ae3fc20716fc977197b85b958497e5889b638ec4299566ab5fcdb")
        packet = protocol.decode(data)
        self.assertEqual(packet["flags"], 16)
        self.assertEqual([frame[0] for frame in packet["frames"]], [96, 97, 98])
        self.assertEqual(int.from_bytes(packet["frames"][0][1][:3], "little", signed=True), -3248570)

    def test_simulator_matches_table_across_cycle(self):
        ref = protocol.reference()
        for sequence in (0, 1, 15, 16, 65537, 2 ** 53 + 1):
            packet = protocol.decode(protocol.make_packet(sequence))
            self.assertEqual(packet["sequence"], sequence)
            for fid, pcm in packet["frames"]:
                self.assertEqual(pcm, ref[(fid % 16) * 384:(fid % 16 + 1) * 384])

    def test_invalid_flags_and_lengths(self):
        good = protocol.make_packet(0)
        for bad in (good[:-1], good + b"x", protocol.make_packet(0, flags=0x20),
                    protocol.make_packet(0, flags=0x40)):
            with self.assertRaises(ValueError):
                protocol.decode(bad)


if __name__ == "__main__":
    unittest.main()
