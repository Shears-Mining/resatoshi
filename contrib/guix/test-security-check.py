#!/usr/bin/env python3
# Copyright (c) 2026 The ReSatoshi developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit-license.php.
"""Check RELRO detection against ELF fixtures with different binding tags."""

import importlib.util
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

import lief

SPEC = importlib.util.spec_from_file_location("security_check", Path(__file__).with_name("security-check.py"))
assert SPEC is not None and SPEC.loader is not None
CHECKER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECKER)


@unittest.skipUnless(sys.platform.startswith("linux") and struct.calcsize("P") == 8 and sys.byteorder == "little",
                     "ELF fixtures require 64-bit little-endian Linux")
class RelroTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        cls.root = Path(cls.directory.name)
        source = cls.root / "fixture.c"
        source.write_text('#include <stdio.h>\nint main(void) { return puts("RELRO"); }\n', encoding="utf8")
        for name, relro in [("full", "relro"), ("no-relro", "norelro")]:
            subprocess.run(["cc", "-fuse-ld=bfd", str(source), f"-Wl,-z,{relro},-z,now", "-o", str(cls.root / name)], check=True)

    def binding_fixture(self, mode):
        path = self.root / "full"
        binary = lief.ELF.parse(path)
        assert binary is not None
        dynamic = binary.get_section(".dynamic")
        assert dynamic is not None
        data = bytearray(path.read_bytes())
        self.assertEqual(data[:6], b"\x7fELF\x02\x01")  # ELF64, little endian
        seen_flags = False
        seen_flags1 = False
        for offset in range(dynamic.offset, dynamic.offset + dynamic.size, 16):
            tag, value = struct.unpack_from("<QQ", data, offset)
            if tag == 30:  # DT_FLAGS
                seen_flags = True
                if mode == "legacy":
                    tag, value = 24, 0  # DT_BIND_NOW
                elif mode != "flags":
                    value &= ~8  # DF_BIND_NOW
            elif tag == 0x6FFFFFFB:  # DT_FLAGS_1
                seen_flags1 = True
                if mode != "flags1":
                    value &= ~1  # DF_1_NOW; keep unrelated flags such as PIE
            struct.pack_into("<QQ", data, offset, tag, value)
        self.assertTrue(seen_flags and seen_flags1)
        target = self.root / mode
        target.write_bytes(data)
        parsed = lief.ELF.parse(target)
        assert parsed is not None
        return parsed

    def test_flags_binding(self):
        self.assertTrue(CHECKER.check_ELF_RELRO(self.binding_fixture("flags")))

    def test_flags1_binding(self):
        self.assertTrue(CHECKER.check_ELF_RELRO(self.binding_fixture("flags1")))

    def test_legacy_binding(self):
        self.assertTrue(CHECKER.check_ELF_RELRO(self.binding_fixture("legacy")))

    def test_relro_without_immediate_binding(self):
        self.assertFalse(CHECKER.check_ELF_RELRO(self.binding_fixture("lazy")))

    def test_immediate_binding_without_relro(self):
        binary = lief.ELF.parse(self.root / "no-relro")
        assert binary is not None
        self.assertFalse(CHECKER.check_ELF_RELRO(binary))


if __name__ == "__main__":
    unittest.main()
