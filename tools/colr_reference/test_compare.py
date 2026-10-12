"""Ensure a truncated, missing or changed readback cannot pass the exact gate."""
import struct
import tempfile
import unittest
from pathlib import Path

from compare import compare


class ComparisonTests(unittest.TestCase):
    def test_complete_readbacks_and_differences(self):
        with tempfile.TemporaryDirectory() as folder:
            old, new = Path(folder) / "old.bin", Path(folder) / "new.bin"
            header = struct.pack("<4I", 12, 32, 1, 1)
            payload = header + bytes([1, 2, 3, 4]) + bytes([9, 9, 9, 9])
            old.write_bytes(payload)
            new.write_bytes(payload)
            self.assertEqual(compare(new, old, 1)["changed_channels"], 0)
            new.write_bytes(header + bytes([1, 2, 4, 4]) + bytes([9, 9, 9, 9]))
            changed = compare(new, old, 1)
            self.assertEqual(changed["changed_channels"], 1)
            self.assertEqual(changed["maximum_channel_delta"], 1)
            for truncated in (b"", payload[:1], payload[:15], payload[:16], payload[:-1]):
                new.write_bytes(truncated)
                with self.assertRaises(ValueError):
                    compare(new, old, 1)
            new.write_bytes(payload)
            old.write_bytes(payload[:-1])
            with self.assertRaises(ValueError):
                compare(new, old, 1)
            old.write_bytes(payload * 2)
            with self.assertRaises(ValueError):
                compare(new, old, 1)
            old.write_bytes(struct.pack("<4I", 13, 32, 1, 1) + payload[16:])
            with self.assertRaises(ValueError):
                compare(new, old, 1)
            with self.assertRaises(ValueError):
                compare(new, expected_records=2)


if __name__ == "__main__":
    unittest.main()
