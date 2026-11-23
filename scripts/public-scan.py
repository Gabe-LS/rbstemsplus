#!/usr/bin/env python3
"""Check an exported source tree. Diagnostics contain locations and types only."""

import argparse
import base64
from collections import defaultdict
from hashlib import sha256
import os
from pathlib import Path
import re
import socket
import subprocess
import sys


PUBLIC_REPOSITORY = "Gabe-LS/rbstemsplus"
GITHUB_REMOTE = re.compile(r"(?:https://github\.com/|ssh[:]//git@github\.com/|git@github\.com:)([^/\s]+/[^/\s]+)", re.I)
OCTET = r"(?:25[0-5]|2[0-4][0-9]|1[0-9]{2}|[1-9]?[0-9])"
PRIVATE_IP = re.compile(
    rf"(?<![\w.])(?:10\.{OCTET}|192\.168|172\.(?:1[6-9]|2[0-9]|3[01])|100\.(?:6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7]))"
    rf"\.{OCTET}\.{OCTET}(?![\w]|\.[0-9])"
)
LABEL = r"[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?"
HOST_PATTERN = rf"{LABEL}(?:\.{LABEL})*"
DOMAIN = re.compile(
    rf"(?<![\w.@$/\\-])(?:{LABEL}\.)+(?:com|net|org|edu|gov|io|dev|info|biz|co|us|uk|"
    r"eu|de|fr|jp|ca|au|ch|nl|ai|me|tv|cloud|xyz|local|lan|internal|corp|test|invalid)(?![\w-]|\.[\w-])",
    re.I,
)
EMAIL = re.compile(rf"(?<![\w.$%+\-])[\w.%+\-]+@(?:{LABEL}\.)+[a-z]{{2,63}}\b", re.I)
URL = re.compile(
    rf"\b(?:https?|ftp|smb|ssh|wss?|afp|vnc)://(?:(?P<user>[\w.-]+)(?::(?P<password>[^@/\s]*))?@)?"
    rf"(?P<host>{HOST_PATTERN})(?![\w\\-]|\.[\w-])",
    re.I,
)
ENDPOINT = re.compile(
    rf"""\b(?:ssh\s+|rbstemsplus\.pc(?:TestSsh|Ssh)\s+)["'`]?"""
    rf"(?P<user>[\w.-]+)@(?P<host>{HOST_PATTERN})(?![\w-]|\.[\w-])",
    re.I,
)
HOST_SETTING = re.compile(
    rf"""(?:--(?:host|hostname|server)\s+|[\w.-]*(?:host(?:name)?|server)\s*=\s*|"""
    rf"""["'][\w.-]*(?:host(?:name)?|server)["']\s*:\s*)["'`]?(?P<host>{HOST_PATTERN})(?=$|[\s:"'`,;])""",
    re.I,
)
UNC_HOST = re.compile(rf"(?<![\w\\])\\{{2,4}}(?:\?\\+UNC\\+)?({HOST_PATTERN})\\+{LABEL}", re.I)
HOME_PATH = re.compile(r"(?<![\w$])/(?:Users|home)/([\w][\w.-]*)", re.I)
WINDOWS_USER = re.compile(
    r"""[a-z]:[\\/]+Users[\\/]+([\w][\w .-]*?)(?=[\\/"'`\r\n]|$)""",
    re.I,
)
PASSWORD_PATTERN = re.compile(
    r"""(?:\b[\w.-]*(?:password|passwd|passphrase)\s*=\s*|"""
    r"""["'][\w.-]*(?:password|passwd|passphrase)["']\s*:\s*|--(?:password|passwd|passphrase)(?:=|\s+))"""
    r"""(?:"([^"\r\n]+)"|'([^'\r\n]+)'|([^\s"'`$,;(){}<>]+))""",
    re.I,
)
SERIAL = re.compile(
    r"""\b(?:[\w.-]*serial(?:number)?|serial\s+number)["']?\s*[:=]\s*["'`]?([a-z0-9_-]{4,})"""
    r"""|\brbstemsplus\.pcT5Serial\s+["'`]?([a-z0-9_-]{4,})""",
    re.I,
)
PGP_LABEL = "PGP PRIVATE KEY BLOCK"
PEM_BEGIN = re.compile(rf"-----BEGIN ((?:[A-Z0-9]+ )*PRIVATE KEY|{PGP_LABEL})-----$")
PEM_BODY = re.compile(r"[A-Za-z0-9+/]+={0,2}")
PEM_HEADER = re.compile(r"(?:Proc-Type:[ \t]*4,ENCRYPTED|DEK-Info:[ \t]*[A-Z0-9-]+,[a-fA-F0-9]+)")
PEM_ESCAPED_EOL = re.compile(r"\\(?:r\\n|n|r)")
LITERAL_PUNCTUATION = " \t,;\\()[]{}"
LITERAL_QUOTES = "\"'`"
LITERAL_OPERATORS = re.escape(LITERAL_PUNCTUATION + "+")
LITERAL_PREFIX = re.compile(rf"^[{LITERAL_OPERATORS}]*[{LITERAL_QUOTES}]+"
                            rf"(?:[{LITERAL_OPERATORS}]+[{LITERAL_QUOTES}]+)*")
PGP_HEADER = re.compile(r"(?:Version|Comment|MessageID|Hash|Charset):.*")
PGP_CHECKSUM = re.compile(r"=[A-Za-z0-9+/]{4}")
PUTTY_BEGIN = re.compile(r"PuTTY-User-Key-File-[23]:[ \t]+\S+")
PUTTY_PRIVATE = re.compile(r"Private-Lines:[ \t]+([1-9][0-9]*)")
NON_TEXT = set()
NO_HOST = {"true", "false", "none", "null"}


def command(*args):
    result = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    return result.stdout.rstrip("\n") if result.returncode == 0 else ""


def local_details(repo):
    def config(key):
        return command("git", "-C", str(repo), "config", "--get", key)

    identities = set()
    full_name = command("id", "-F")
    for name in (full_name, *full_name.split(), config("user.name")):
        if len(name) >= 5 and name.casefold() != "gabe-ls":
            identities.add(("personal-name", name))
    email = config("user.email")
    if "@" in email and email != config("rbstemsplus.publicEmail"):
        identities.add(("email", email))
    user = command("id", "-un")
    if user and user != "root":
        identities.add(("local-username", user))
    local_host = socket.gethostname()
    if local_host and local_host not in {"localhost", "localhost.localdomain"}:
        identities.add(("hostname", local_host))
    origin = GITHUB_REMOTE.fullmatch(config("remote.origin.url").rstrip("/"))
    if origin:
        slug = origin[1].removesuffix(".git")
        if slug.casefold() != PUBLIC_REPOSITORY.casefold():
            identities.update({("private-repository", slug), ("private-repository", slug.split("/")[1])})
    for key in ("pcSsh", "pcTestSsh"):
        user, sep, host = config("rbstemsplus." + key).rpartition("@")
        if sep:
            identities.add(("windows-account", user))
            identities.add(("hostname", host))
    for key in ("ntfyHost", "nasHost", "pcTestServer"):
        value = config("rbstemsplus." + key)
        if value:
            url = URL.search(value)
            host, _, port = value.rpartition(":")
            identities.add(("hostname", url["host"] if url else host if port.isdecimal() else value))
    serial = config("rbstemsplus.pcT5Serial")
    if serial:
        identities.add(("device-serial", serial))
    for value in command("git", "-C", str(repo), "config", "--get-all", "rbstemsplus.pcProtectedDisks").splitlines():
        try:
            serial, sep, _ = base64.b64decode(value, validate=True).decode().partition("|")
        except ValueError:
            continue
        if sep and serial:
            identities.update({("device-serial", serial), ("device-serial", value)})
    return [(kind, value, re.compile(r"(?<![\w.-])" + re.escape(value) + r"(?![\w-])", re.I))
            for kind, value in sorted(identities) if value]


def candidates(line, identities):
    emails = {m.span(): m[0] for m in EMAIL.finditer(line)}
    private_ips = {m[0] for m in PRIVATE_IP.finditer(line)}
    for value in private_ips:
        yield "private-ip", value
    for match in HOME_PATH.finditer(line):
        yield "home-path", match[0]
    for match in WINDOWS_USER.finditer(line):
        yield "windows-account", match[1]
    endpoints = list(ENDPOINT.finditer(line))
    for m in endpoints:
        yield "windows-account", m["user"]
        emails.pop(m.span("user")[:1] + m.span("host")[1:], None)
    for m in URL.finditer(line):
        if m["user"]:
            emails.pop((m.start("user"), m.end("host")), None)
            yield "windows-account", m["user"]
        if m["password"]:
            yield "password-literal", m["password"]
    for value in emails.values():
        yield "email", value
    hosts = {m["host"] for pattern in (URL, ENDPOINT, HOST_SETTING) for m in pattern.finditer(line)}
    hosts.update(m[1] for m in UNC_HOST.finditer(line))
    for value in hosts - private_ips:
        if value.lower() not in NO_HOST:
            yield "hostname", value
    for m in DOMAIN.finditer(line):
        yield "hostname", m[0]
    for m in PASSWORD_PATTERN.finditer(line):
        value = next(v for v in m.groups() if v is not None)
        if value and not value.startswith(("$", "%", "<", "\\")):
            yield "password-literal", value
    for m in SERIAL.finditer(line):
        value = m[1] or m[2]
        if any(c.isdigit() for c in value):
            yield "device-serial", value
    for kind, value, pattern in identities:
        if pattern.search(line):
            yield kind, value


def read_allowlist(path):
    result = defaultdict(set)
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) < 2 or not all(fields):
            raise ValueError(f"invalid allowlist entry on line {number}")
        kind, value, *scope = fields
        result[kind, value.casefold()].update(scope or [""])
    return result


def armor_fragment(fragment):
    fragment = fragment.replace(r"\t", "\t").replace(r"\/", "/").strip(LITERAL_PUNCTUATION)
    # A plus outside quotes may join literals. A plus inside is base64 data.
    suffix = fragment.rstrip(LITERAL_PUNCTUATION + "+")
    if suffix and suffix[-1] in LITERAL_QUOTES:
        fragment = suffix.rstrip(LITERAL_PUNCTUATION + LITERAL_QUOTES)
    return LITERAL_PREFIX.sub("", fragment).lstrip(LITERAL_PUNCTUATION)


def has_putty_private_body(lines):
    for index, line in enumerate(lines, 1):
        section = PUTTY_PRIVATE.fullmatch(line.strip())
        if section:
            count = section[1]
            if len(count) > len(str(len(lines))):
                return False
            end = index + int(count)
            return end <= len(lines) and all(PEM_BODY.fullmatch(body.strip()) for body in lines[index:end])
    return False


def private_key_lines(lines):
    if lines and PUTTY_BEGIN.fullmatch(lines[0].removeprefix("\ufeff").strip()) and has_putty_private_body(lines):
        yield 1
    # A block counts once it holds 64 base64 characters, with or without its END line: a truncated
    # paste, or text after END on the same line, still leaks the key material before it.
    label, size = None, 0
    for line_number, line in enumerate(lines, 1):
        if label is None and "-----BEGIN " not in line:
            continue
        for fragment in PEM_ESCAPED_EOL.split(line):
            fragment = armor_fragment(fragment)
            begin = PEM_BEGIN.search(fragment)
            if begin:
                if label and size >= 64:
                    yield start
                label, start, size, checksum = begin[1], line_number, 0, False
                header = PGP_HEADER if label == PGP_LABEL else PEM_HEADER
            elif label and fragment:
                if fragment == f"-----END {label}-----":
                    # Count base64 characters without padding. A 48-byte PKCS#8 key needs 64.
                    if size >= 64:
                        yield start
                    label = None
                elif not checksum and PEM_BODY.fullmatch(fragment):
                    size += len(fragment.rstrip("="))
                elif label == PGP_LABEL and size and not checksum and PGP_CHECKSUM.fullmatch(fragment):
                    checksum = True
                elif size or not header.fullmatch(fragment):
                    if size >= 64:
                        yield start
                    label = None
    if label and size >= 64:
        yield start


def scan(stage, allow, identities):
    def hits(line, relative):
        for kind, value in candidates(line, identities):
            value = "sha256:" + sha256(value.encode()).hexdigest() if kind == "password-literal" else value.casefold()
            scopes = allow.get((kind, value), ())
            if not any(
                not scope or scope == relative or (scope.endswith("/") and relative.startswith(scope))
                for scope in scopes
            ):
                yield kind

    for path in sorted(files(stage)):
        relative = path.relative_to(stage).as_posix()
        path_hits = set(hits(relative, relative))
        display = "[path redacted]" if path_hits else relative
        for kind in sorted(path_hits):
            yield display, 0, kind
        if path.is_symlink():
            lines = [str(path.readlink())]
        elif path.is_file() and path.suffix.lower() not in NON_TEXT:
            data = path.read_bytes()
            content = source_text(data)
            if content is None:
                if relative not in allow.get(("opaque", sha256(data).hexdigest()), ()):
                    yield display, 0, "opaque"
                continue
            lines = content.splitlines()
        else:
            continue
        key_lines = set(private_key_lines(lines))
        for line_number, line in enumerate(lines, 1):
            kinds = set(hits(line, relative))
            if line_number in key_lines:
                kinds.add("private-key")
            for kind in sorted(kinds):
                yield display, line_number, kind


def files(stage):
    def onerror(error):
        raise error

    for directory, subdirs, names in os.walk(stage, onerror=onerror):
        yield from (Path(directory, name) for name in names)
        for name in subdirs:
            path = Path(directory, name)
            if path.is_symlink():
                yield path


def source_text(data):
    if data.startswith((b"\xff\xfe", b"\xfe\xff")):
        encoding = "utf-16"
    elif b"\0" not in data:
        return data.decode("utf-8", errors="replace")
    elif len(data) % 2:
        return None
    elif 10 * data[1::2].count(0) >= 9 * (len(data) // 2):
        encoding = "utf-16-le"
    elif 10 * data[::2].count(0) >= 9 * (len(data) // 2):
        encoding = "utf-16-be"
    else:
        return None
    try:
        text = data.decode(encoding)
    except UnicodeDecodeError:
        return None
    # A pair with two zero bytes decodes to a NUL, so it cannot qualify as text.
    return text if "\0" not in text else None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", type=Path)
    parser.add_argument("allowlist", type=Path)
    parser.add_argument("repo", type=Path)
    args = parser.parse_args()
    try:
        allow = read_allowlist(args.allowlist)
        identities = local_details(args.repo)
        found = False
        for path, line, kind in scan(args.stage, allow, identities):
            print(f"{path}:{line}: [{kind}]", file=sys.stderr)
            found = True
        return int(found)
    except (ValueError, OSError):
        print("public scan: could not read the export or allowlist", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
