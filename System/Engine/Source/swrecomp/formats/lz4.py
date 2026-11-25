"""LZ4 block-format decompression.

NSO0 execƒutable segments are compressed with the raw LZ4 *block* format (no
frame header), as verified against the per-segment SHA-256 hashes stored in the
NSO header.  Pure-Python implementation so the toolchain has no third-party
dependencies.
"""

from __future__ import annotations

from ..errors import FormatError


def decompress_block(src: bytes, expected_size: int) -> bytes:
    """Decompress an LZ4 block.

    ``expected_size`` is the exact decompressed size; a mismatch is an error,
    because NSO headers state it precisely and silently accepting a wrong size
    would corrupt every downstream offset.
    """
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if i >= n:
                    raise FormatError("lz4: truncated literal-length")
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if i + lit > n:
            raise FormatError("lz4: literal run past end of input")
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break  # last sequence has no match
        if i + 2 > n:
            raise FormatError("lz4: truncated match offset")
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0:
            raise FormatError("lz4: zero match offset")
        if off > len(out):
            raise FormatError("lz4: match offset %d before output start" % off)
        mlen = token & 0xF
        if mlen == 15:
            while True:
                if i >= n:
                    raise FormatError("lz4: truncated match-length")
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        mlen += 4
        start = len(out) - off
        if off >= mlen:
            out += out[start:start + mlen]
        else:  # overlapping copy must be byte-wise
            for k in range(mlen):
                out.append(out[start + k])
        if len(out) > expected_size:
            raise FormatError("lz4: output overflow (%d > %d)" % (len(out), expected_size))
    if len(out) != expected_size:
        raise FormatError("lz4: size mismatch %d != %d" % (len(out), expected_size))
    return bytes(out)


def compress_block(src: bytes) -> bytes:
    """Minimal LZ4 block compressor (literal-only + greedy 4-byte matches).

    Only used by test fixtures; the recompiler never needs to compress.
    """
    out = bytearray()
    i = 0
    n = len(src)
    anchor = 0

    def emit_seq(lit: bytes, offset: int, mlen: int) -> None:
        lit_len = len(lit)
        token_hi = 15 if lit_len >= 15 else lit_len
        token_lo = 15 if mlen - 4 >= 15 else mlen - 4
        out.append((token_hi << 4) | token_lo)
        if lit_len >= 15:
            rem = lit_len - 15
            while rem >= 255:
                out.append(255)
                rem -= 255
            out.append(rem)
        out.extend(lit)
        if offset:
            out.append(offset & 0xFF)
            out.append((offset >> 8) & 0xFF)
            if mlen - 4 >= 15:
                rem = mlen - 4 - 15
                while rem >= 255:
                    out.append(255)
                    rem -= 255
                out.append(rem)

    table: dict[bytes, int] = {}
    while i + 4 <= n:
        key = src[i:i + 4]
        cand = table.get(key)
        table[key] = i
        if cand is not None and i - cand <= 0xFFFF:
            mlen = 4
            while i + mlen < n and src[cand + mlen] == src[i + mlen] and mlen < 512:
                mlen += 1
            emit_seq(src[anchor:i], i - cand, mlen)
            i += mlen
            anchor = i
            continue
        i += 1
    emit_seq(src[anchor:n], 0, 0)
    return bytes(out)
