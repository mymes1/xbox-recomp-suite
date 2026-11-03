import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
import sonic_heroes


def make_xbe(entry=0x10000, debug=False, title=0x4F48534E, build=4531):
    header = bytearray(0x200)
    header[0:4] = b"XBEH"
    struct.pack_into("<I", header, 0x104, 0x10000)  # base
    struct.pack_into("<I", header, 0x108, 0x200)    # header size
    struct.pack_into("<I", header, 0x10C, 0x1000)   # image size
    struct.pack_into("<I", header, 0x118, 0x10180)  # certificate
    struct.pack_into("<I", header, 0x11C, 1)        # section count
    struct.pack_into("<I", header, 0x120, 0x10100)  # section headers
    struct.pack_into("<I", header, 0x124, 0)        # init flags
    if debug:
        struct.pack_into("<I", header, 0x128, entry ^ sonic_heroes.DEBUG_ENTRY_KEY)
        struct.pack_into("<I", header, 0x158, entry ^ sonic_heroes.DEBUG_THUNK_KEY)
    else:
        struct.pack_into("<I", header, 0x128, entry ^ sonic_heroes.RETAIL_ENTRY_KEY)
        struct.pack_into("<I", header, 0x158, entry ^ sonic_heroes.RETAIL_THUNK_KEY)
    struct.pack_into("<I", header, 0x12C, 0)        # tls
    struct.pack_into("<I", header, 0x160, 1)        # library count
    struct.pack_into("<I", header, 0x164, 0x10140)  # libraries
    struct.pack_into("<I", header, 0x188, title)    # title id at certificate + 8
    # section header
    struct.pack_into("<I", header, 0x100, 4)        # executable
    struct.pack_into("<I", header, 0x104, 0x10000)
    struct.pack_into("<I", header, 0x108, 0x1000)
    struct.pack_into("<I", header, 0x10C, 0x200)
    struct.pack_into("<I", header, 0x110, 0x1000)
    struct.pack_into("<I", header, 0x114, 0x10190)  # name address
    header[0x190:0x196] = b".text\0"
    # library
    header[0x140:0x144] = b"D3D8"
    struct.pack_into("<HHHH", header, 0x148, 4, 0, build, 0)
    return bytes(header)


class XbeParserTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, data, name="default.xbe"):
        path = self.root / name
        path.write_bytes(data)
        return path

    def test_retail_entry_and_sections(self):
        xbe = sonic_heroes.Xbe(self.write(make_xbe()))
        self.assertEqual(xbe.entry, 0x10000)
        self.assertEqual(xbe.title_id, 0x4F48534E)
        self.assertEqual(xbe.debug, False)
        self.assertEqual([s["name"] for s in xbe.sections], [".text"])
        self.assertEqual(xbe.executable_sections(), [".text"])
        self.assertEqual(xbe.library_builds(), [4531])
        self.assertEqual(xbe.choose_xdk(), (4531, True))

    def test_debug_entry_key(self):
        xbe = sonic_heroes.Xbe(self.write(make_xbe(debug=True)))
        self.assertEqual(xbe.entry, 0x10000)
        self.assertEqual(xbe.debug, True)

    def test_rejects_non_xbe(self):
        path = self.write(b"not an xbe" + bytes(0x200))
        with self.assertRaises(ValueError):
            sonic_heroes.Xbe(path)

    def test_generated_config(self):
        xbe = sonic_heroes.Xbe(self.write(make_xbe()))
        out = self.root / "game.toml"
        xdk, exact = sonic_heroes.write_config(xbe, out)
        self.assertEqual(xdk, 4531)
        self.assertTrue(exact)
        text = out.read_text()
        self.assertIn("entry = 0x10000", text)
        self.assertIn("title = 0x4F48534E", text)
        self.assertIn("xdk = 4531", text)
        self.assertIn('code_sections = [".text"]', text)
        self.assertIn("[symbols]", text)
        self.assertIn("[data]", text)

    def test_xdk_override_is_not_exact(self):
        xbe = sonic_heroes.Xbe(self.write(make_xbe()))
        out = self.root / "game.toml"
        xdk, exact = sonic_heroes.write_config(xbe, out, 4721)
        self.assertEqual(xdk, 4721)
        self.assertFalse(exact)
        self.assertIn("xdk = 4721", out.read_text())

    def test_rebuild_keeps_the_symbol_tables(self):
        xbe = sonic_heroes.Xbe(self.write(make_xbe()))
        out = self.root / "game.toml"
        sonic_heroes.write_config(xbe, out)
        text = out.read_text()
        text = text.replace(
            "[symbols]\n",
            "[symbols]\nD3D_Device                  = 0x279B50  # kept by hand\n")
        text = text.replace("[data]\n", "[data]\nXPP_GamepadType             = 0x27CF4C\n")
        text = text.replace("[functions]\n", '[functions]\n0x19E004 = ""\n')
        out.write_text(text)
        sonic_heroes.write_config(xbe, out)
        rebuilt = out.read_text()
        self.assertIn("D3D_Device                  = 0x279B50  # kept by hand", rebuilt)
        self.assertIn("XPP_GamepadType             = 0x27CF4C", rebuilt)
        self.assertIn('0x19E004 = ""', rebuilt)
        # the generator still owns [xbe], and a caption is not copied with the table above it
        self.assertEqual(rebuilt.count("entry = 0x10000"), 1)
        self.assertEqual(rebuilt.count("# functions the host replaces or calls by name"), 1)

    def test_rebuild_drops_tables_of_another_executable(self):
        out = self.root / "game.toml"
        out.write_text(
            "[xbe]\n"
            'path = "assets/default.xbe"\n'
            "title = 0x11111111\n"
            "entry = 0x20000\n"
            "xdk = 5849\n"
            'out = "generated/game"\n'
            "partitions = 8\n"
            'code_sections = [".text"]\n'
            "\n[symbols]\nD3D_Device = 0x279B50\n\n[data]\n")
        xbe = sonic_heroes.Xbe(self.write(make_xbe()))
        sonic_heroes.write_config(xbe, out)
        rebuilt = out.read_text()
        self.assertNotIn("D3D_Device", rebuilt)
        self.assertIn("title = 0x4F48534E", rebuilt)

    def test_generated_config_has_the_kept_tables(self):
        xbe = sonic_heroes.Xbe(self.write(make_xbe()))
        out = self.root / "game.toml"
        sonic_heroes.write_config(xbe, out)
        text = out.read_text()
        for table in ("[functions]", "[symbols]", "[data]"):
            self.assertIn(table, text)
        self.assertNotIn("[abi]", text)

    def test_partitions_are_clamped(self):
        xbe = sonic_heroes.Xbe(self.write(make_xbe()))
        self.assertGreaterEqual(xbe.partitions(), 8)
        self.assertLessEqual(xbe.partitions(), 64)


if __name__ == "__main__":
    unittest.main()
