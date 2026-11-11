"""LZ4 block decompression (pure Python, no dependencies).

NSO segments are raw LZ4 blocks (no frame header). The decoder works on whole
sequences and uses slice copies, so a 60 MB text segment takes seconds, not
minutes.
"""


class LZ4Error(ValueError):
    pass


def decompress_block(src, out_size):
    src = memoryview(bytes(src))
    n = len(src)
    out = bytearray()
    i = 0
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if i >= n:
                    raise LZ4Error('truncated literal length')
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if lit:
            if i + lit > n:
                raise LZ4Error('literal run past end of input')
            out += src[i:i + lit]
            i += lit
        if i >= n:
            break  # last sequence carries literals only
        if i + 2 > n:
            raise LZ4Error('truncated match offset')
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0 or off > len(out):
            raise LZ4Error(f'bad match offset {off} at output {len(out):#x}')
        mlen = (token & 15) + 4
        if (token & 15) == 15:
            while True:
                if i >= n:
                    raise LZ4Error('truncated match length')
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        start = len(out) - off
        if off >= mlen:
            out += out[start:start + mlen]
        else:  # overlapping copy: repeat the period
            pat = bytes(out[start:])
            reps, rem = divmod(mlen, off)
            out += pat * reps + pat[:rem]
    if len(out) != out_size:
        raise LZ4Error(f'decompressed to {len(out):#x} bytes, expected {out_size:#x}')
    return out
