import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "launcher"))
import pla_edit  # noqa: E402


def test_pk_crypt_roundtrip_all_shuffles():
    r = random.Random(1)
    for sv in range(32):
        plain = bytearray(r.randbytes(pla_edit.PK_PARTY))
        pv = (r.getrandbits(32) & ~(31 << 13)) | (sv << 13)
        plain[0:4] = pv.to_bytes(4, "little")
        enc = pla_edit.pk_encrypt(bytes(plain))
        assert enc != bytes(plain)
        assert pla_edit.pk_decrypt(enc) == plain


def test_shiny_toggle():
    m = pla_edit.Pokemon(bytearray(pla_edit.PK_PARTY))
    m._w16(0x0C, 12345)
    m._w16(0x0E, 54321)
    m.pid = 0xDEADBEEF
    m.shiny = True
    assert m.shiny
    m.shiny = False
    assert not m.shiny
