#!/usr/bin/env python3
"""gguf_add_kv.py — add one uint32 metadata key to a GGUF file, in place.

Usage: gguf_add_kv.py <file.gguf> <key> <uint32-value>

Exits 0 without touching the file when the key is already present.
Tensor data offsets are relative to the aligned data section, so only the
header is rewritten and the padding recomputed; the tensor blob is copied
verbatim. Used by the fetch-models scripts to repair third-party GGUF
conversions that predate keys newer llama.cpp revisions require (e.g.
tokenizer.ggml.token_type_count on BERT-style embedders).
"""

import os
import struct
import sys

GGUF_TYPE_U32 = 4
SIZES = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 4, 7: 1, 10: 8, 11: 8, 12: 8}


def rd_str(f):
    (n,) = struct.unpack("<Q", f.read(8))
    return f.read(n)


def skip_val(f, t):
    if t == 8:  # string
        rd_str(f)
    elif t == 9:  # array
        et, n = struct.unpack("<IQ", f.read(12))
        for _ in range(n):
            skip_val(f, et)
    else:
        f.read(SIZES[t])


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__.strip())
    path, key, value = sys.argv[1], sys.argv[2].encode(), int(sys.argv[3])

    with open(path, "rb") as f:
        if f.read(4) != b"GGUF":
            sys.exit(f"{path}: not a GGUF file")
        (ver,) = struct.unpack("<I", f.read(4))
        n_tensors, n_kv = struct.unpack("<QQ", f.read(16))

        align = 32
        for _ in range(n_kv):
            k = rd_str(f)
            (t,) = struct.unpack("<I", f.read(4))
            pos = f.tell()
            skip_val(f, t)
            if k == key:
                print(f"{path}: {key.decode()} already present")
                return
            if k == b"general.alignment":
                f.seek(pos)
                (align,) = struct.unpack("<I", f.read(4))
        kv_end = f.tell()

        for _ in range(n_tensors):
            rd_str(f)
            (nd,) = struct.unpack("<I", f.read(4))
            f.read(8 * nd + 4 + 8)  # dims + dtype + offset
        header_end = f.tell()
        data_start = (header_end + align - 1) // align * align

        f.seek(0)
        header = f.read(header_end)
        f.seek(data_start)

        new_kv = (
            struct.pack("<Q", len(key)) + key
            + struct.pack("<II", GGUF_TYPE_U32, value)
        )
        tmp = path + ".tmp"
        with open(tmp, "wb") as out:
            out.write(b"GGUF" + struct.pack("<IQQ", ver, n_tensors, n_kv + 1))
            out.write(header[24:kv_end])
            out.write(new_kv)
            out.write(header[kv_end:header_end])
            pad = (out.tell() + align - 1) // align * align - out.tell()
            out.write(b"\x00" * pad)
            while True:
                chunk = f.read(1 << 22)
                if not chunk:
                    break
                out.write(chunk)
    os.replace(tmp, path)
    print(f"{path}: added {key.decode()} = {value}")


if __name__ == "__main__":
    main()
