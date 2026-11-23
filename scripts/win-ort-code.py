#!/usr/bin/env python3
"""Check Windows ORT bindings and print their shared PE code digest."""

import argparse
import hashlib
from pathlib import Path
import re
import struct
import sys


CERT_MAX = 64 * 1024
SHA256 = re.compile(r"[0-9a-f]{64}")
UNMEASURED = "UNMEASURED_PE_CODE_SHA256"
UNBOUND = "0" * 64


def u16(image, at):
    return struct.unpack_from("<H", image, at)[0]


def u32(image, at):
    return struct.unpack_from("<I", image, at)[0]


def pe_code_sha256(image):
    """Hash the retained ranges using the same checks as win/c/pe_code.c."""
    if len(image) < 64 or image[:2] != b"MZ":
        raise ValueError("missing DOS header")
    pe = u32(image, 0x3c)
    optional = pe + 24
    if pe < 64 or optional + 2 > len(image) or image[pe:pe + 4] != b"PE\0\0":
        raise ValueError("missing PE header")
    count = u16(image, pe + 6)
    opt_size = u16(image, pe + 20)
    directories = {0x10b: 96, 0x20b: 112}.get(u16(image, optional))
    if not 1 <= count <= 96 or directories is None:
        raise ValueError("invalid section count or optional header type")
    cert_entry = directories + 4 * 8
    required = cert_entry + 8
    if opt_size < required or optional + required > len(image):
        raise ValueError("truncated optional header")
    dir_count = u32(image, optional + directories - 4)
    if not 5 <= dir_count <= (opt_size - directories) // 8:
        raise ValueError("invalid data directory count")

    sections = optional + opt_size
    headers = u32(image, optional + 60)
    if not sections + count * 40 <= headers <= len(image):
        raise ValueError("invalid PE header size")
    checksum = optional + 64
    entry = optional + cert_entry
    cert_at, cert_size = struct.unpack_from("<II", image, entry)
    if cert_at or cert_size:
        if cert_at < headers or cert_at % 8 or not 0 < cert_size <= CERT_MAX or cert_at + cert_size != len(image):
            raise ValueError("certificate table must be after the headers, at EOF and at most 64 KiB")
        limit = cert_at
    else:
        limit = len(image)

    raw_ranges = []
    for i in range(count):
        size, start = struct.unpack_from("<II", image, sections + i * 40 + 16)
        if not size:
            continue
        end = start + size
        if start < headers or end > limit or any(start < prior_end and prior_start < end for prior_start, prior_end in raw_ranges):
            raise ValueError("overlapping or out-of-bounds section data")
        raw_ranges.append((start, end))

    at = limit
    while at < len(image):
        if len(image) - at < 8:
            raise ValueError("truncated certificate record")
        length = u32(image, at)
        aligned = (length + 7) & ~7
        if length < 8 or aligned > len(image) - at:
            raise ValueError("invalid certificate record length")
        at += aligned

    digest = hashlib.sha256()
    data = memoryview(image)
    digest.update(data[:checksum])
    digest.update(data[checksum + 4:entry])
    digest.update(data[entry + 8:limit])
    return digest.hexdigest()


def parse_bindings(bindings, recorded):
    if len(bindings) > 64:
        raise ValueError("at most 64 ORT bindings are supported")
    versions = set()
    parsed = []
    version_pattern = r"[0-9]{1,9}(?:\.[0-9]{1,9}){2}" if recorded else r"[0-9]+(?:\.[0-9]+){2,}"
    for binding in bindings:
        version, _, hashes = binding.partition("=")
        expected, separator, code = hashes.partition(":")
        if (len(version) >= 128 or not re.fullmatch(version_pattern, version)
                or not SHA256.fullmatch(expected) or bool(separator) != recorded):
            form = "VERSION=SHA256:PE_CODE_SHA256" if recorded else "VERSION=SHA256"
            raise ValueError(f"{binding!r} must be {form}")
        if version in versions:
            raise ValueError(f"duplicate ORT binding for rekordbox {version}")
        versions.add(version)
        parsed.append((version, expected, code))
    return parsed


def shared_code(originals, bindings):
    first_version = shared = None
    for version, expected, _ in bindings:
        path = originals / version / "onnxruntime.dll"
        if not path.is_file():
            raise ValueError(f"missing original: {path}")
        image = path.read_bytes()
        if hashlib.sha256(image).hexdigest() != expected:
            raise ValueError(f"{path}: SHA-256 does not match the binding")
        try:
            code = pe_code_sha256(image)
        except ValueError as error:
            raise ValueError(f"{path}: {error}") from error
        if shared is None:
            first_version, shared = version, code
        elif code != shared:
            raise ValueError(f"rekordbox {first_version} and {version} originals have different PE code digests")
    return shared


def recorded_code(bindings, test, originals):
    first_version = shared = None
    unmeasured = []
    for version, _, code in bindings:
        if code == UNMEASURED:
            unmeasured.append(version)
            continue
        if not SHA256.fullmatch(code) or code == UNBOUND:
            raise ValueError(f"rekordbox {version}: invalid PE code digest")
        if shared is None:
            first_version, shared = version, code
        elif code != shared:
            raise ValueError(f"rekordbox {first_version} and {version} have different PE code digests in the bindings")

    if unmeasured:
        message = f"{UNMEASURED} for rekordbox {', '.join(unmeasured)}"
        if not test or originals is not None:
            raise ValueError(message)
        print(f"win-ort-code.py: warning: {message}; Stems Cache install bindings are inactive in this TEST build",
              file=sys.stderr)
        return UNBOUND
    if originals is not None and shared_code(originals, bindings) != shared:
        raise ValueError("the originals' PE code digest does not match the recorded PE code digest")
    return shared


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--originals", type=Path, help="directory containing VERSION/onnxruntime.dll")
    parser.add_argument("--recorded", action="store_true", help="check VERSION=SHA256:PE_CODE_SHA256 entries")
    parser.add_argument("--test", action="store_true", help="allow unmeasured recorded digests for TEST builds")
    parser.add_argument("binding", nargs="+", metavar="BINDING")
    args = parser.parse_args()
    if not args.recorded and (args.originals is None or args.test):
        parser.error("use --recorded or supply --originals for VERSION=SHA256 entries")
    try:
        bindings = parse_bindings(args.binding, args.recorded)
        print(recorded_code(bindings, args.test, args.originals) if args.recorded else shared_code(args.originals, bindings))
    except (OSError, ValueError) as error:
        print(f"win-ort-code.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
