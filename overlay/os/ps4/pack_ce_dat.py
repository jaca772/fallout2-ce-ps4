#!/usr/bin/env python3
"""Packs a directory (default: files/ce.dat) into a Fallout 2 DAT archive
(default: os/ps4/assets/ce.dat), matching tools/dat_tool.cc byte layout."""

import os
import struct
import sys
import zlib


def pack_fo2_dat(input_dir: str, output_dat: str) -> None:
    if not os.path.isdir(input_dir):
        raise SystemExit(f"Input directory does not exist: {input_dir}")

    entries = []
    seen_lower = {}
    for root, dirs, files in os.walk(input_dir):
        for fname in files:
            native_path = os.path.join(root, fname)
            if not os.path.isfile(native_path):
                continue
            rel_path = os.path.relpath(native_path, input_dir)
            archive_path = rel_path.replace("/", "\\")
            lower_key = archive_path.lower()
            if lower_key in seen_lower:
                raise SystemExit(
                    f"Duplicate case-insensitive path: {seen_lower[lower_key]} vs {archive_path}"
                )
            seen_lower[lower_key] = archive_path
            entries.append((lower_key, archive_path, native_path))

    entries.sort(key=lambda item: item[0])

    os.makedirs(os.path.dirname(os.path.abspath(output_dat)), exist_ok=True)

    dir_records = []
    data_offset = 0
    uncompressed_total = 0
    compressed_count = 0

    with open(output_dat, "wb") as out:
        for _, archive_path, native_path in entries:
            with open(native_path, "rb") as f:
                raw_data = f.read()

            uncompressed_size = len(raw_data)
            uncompressed_total += uncompressed_size

            use_compressed = False
            stored_data = raw_data
            if uncompressed_size > 0:
                comp_data = zlib.compress(raw_data, zlib.Z_DEFAULT_COMPRESSION)
                if len(comp_data) < uncompressed_size:
                    use_compressed = True
                    stored_data = comp_data
                    compressed_count += 1

            packed_size = len(stored_data)
            out.write(stored_data)

            path_bytes = archive_path.encode("ascii")
            record = (
                struct.pack("<I", len(path_bytes))
                + path_bytes
                + struct.pack(
                    "<BIII",
                    1 if use_compressed else 0,
                    uncompressed_size,
                    packed_size,
                    data_offset,
                )
            )
            dir_records.append(record)
            data_offset += packed_size

        dir_blob = struct.pack("<I", len(dir_records)) + b"".join(dir_records)
        entries_data_size = len(dir_blob)
        dbase_data_size = data_offset + entries_data_size + 8

        out.write(dir_blob)
        out.write(struct.pack("<II", entries_data_size, dbase_data_size))

    print(
        f"Created {output_dat}: {len(entries)} entries "
        f"({compressed_count} compressed, {uncompressed_total} raw bytes -> {dbase_data_size} archive bytes)"
    )


def main() -> None:
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    in_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo_root, "files", "ce.dat")
    out_dat = (
        sys.argv[2]
        if len(sys.argv) > 2
        else os.path.join(repo_root, "os", "ps4", "assets", "ce.dat")
    )
    pack_fo2_dat(in_dir, out_dat)


if __name__ == "__main__":
    main()
