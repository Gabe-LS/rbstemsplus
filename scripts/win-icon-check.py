#!/usr/bin/env python3
"""Require the Windows app's complete icon group in a PE's resource table."""
import struct
import sys
from pathlib import Path


RT_ICON = 3
RT_GROUP_ICON = 14
DIRECTORY = 0x80000000
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
PNG_256 = b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sII", 13, b"IHDR", 256, 256)
GROUP_ENTRY = struct.Struct("<BBBBHHIH")


def has_icon(pe):
    header, = struct.unpack_from("<I", pe, 0x3C)
    if pe[:2] != b"MZ" or pe[header:header + 4] != b"PE\0\0":
        raise ValueError("invalid PE signature")

    sections, = struct.unpack_from("<H", pe, header + 6)
    optional_size, = struct.unpack_from("<H", pe, header + 20)
    optional = header + 24
    magic, = struct.unpack_from("<H", pe, optional)
    directories = {0x10B: 96, 0x20B: 112}.get(magic)  # PE32 and PE32+
    if directories is None or optional_size < directories + 24:
        raise ValueError("invalid PE optional header")
    count, = struct.unpack_from("<I", pe, optional + directories - 4)
    rva, size = struct.unpack_from("<II", pe, optional + directories + 16)
    if count < 3 or not rva or not size:
        return False

    section_table = optional + optional_size
    ranges = [struct.unpack_from("<III", pe, section_table + 40 * i + 12)
              for i in range(sections)]  # virtual address, raw size, raw file offset

    def at_rva(address, length):
        for start, raw_size, offset in ranges:
            delta = address - start
            if 0 <= delta and delta + length <= raw_size and offset + delta + length <= len(pe):
                return pe[offset + delta:offset + delta + length]
        raise ValueError("resource RVA is outside the file's sections")

    table = at_rva(rva, size)

    def subdirectory(entry):
        if not entry & DIRECTORY:
            raise ValueError("missing resource subdirectory")
        offset = entry & ~DIRECTORY
        named, numbered = struct.unpack_from("<HH", table, offset + 12)
        return dict(struct.unpack_from("<II", table, offset + 16 + i * 8)
                    for i in range(named + numbered))

    types = subdirectory(DIRECTORY)
    if RT_ICON not in types or RT_GROUP_ICON not in types:
        return False

    def resources(kind):
        for name, entry in subdirectory(types[kind]).items():
            for language, data in subdirectory(entry).items():
                address, length = struct.unpack_from("<II", table, data)
                yield (name, language), at_rva(address, length)

    images = dict(resources(RT_ICON))
    for (_, language), group in resources(RT_GROUP_ICON):
        if len(group) != 6 + len(SIZES) * GROUP_ENTRY.size:
            continue
        if struct.unpack_from("<HHH", group) != (0, 1, len(SIZES)):
            continue
        sizes = []
        for width, height, _, _, _, _, length, image_id in GROUP_ENTRY.iter_unpack(group[6:]):
            image = images.get((image_id, language))
            if width != height or not image or len(image) != length:
                break
            if width == 0 and not image.startswith(PNG_256):
                break
            sizes.append(width or 256)
        if tuple(sorted(sizes)) == SIZES:
            return True
    return False


if __name__ == "__main__":
    path = Path(sys.argv[1])
    try:
        if not has_icon(path.read_bytes()):
            sys.exit(f"{path}: missing the complete PE icon group (16–256 px, 256 px PNG)")
    except (OSError, ValueError, struct.error) as error:
        sys.exit(f"{path}: cannot read PE icon resources: {error}")
    print(f"{path.name}: PE icon group verified (9 sizes, 256 px PNG)")
