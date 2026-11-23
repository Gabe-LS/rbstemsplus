#!/bin/bash
# Run the real export gate against a local, empty public remote.
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
rel="${1:-$root/scripts/release.sh}"
t="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-public-scan-test.XXXXXX")"
trap 'rm -rf "$t"' EXIT
t="$(cd "$t" && pwd -P)"
export TMPDIR="$t" GIT_ALLOW_PROTOCOL=file GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1
export PATH="$t/bin:$PATH"
mkdir -p "$t/bin" "$t/remote/Gabe-LS" "$t/dev/scripts" "$t/dev/ops-fixture"
printf '#!/bin/sh\nexit 1\n' > "$t/bin/gh"
cat > "$t/bin/id" <<'EOF'
#!/bin/sh
case "$1" in
  -F) printf '%s\n' 'Fixture Releaseperson' ;;
  -un) printf '%s\n' 'fixturelocaluser' ;;
  *) exit 1 ;;
esac
EOF
chmod +x "$t/bin/gh" "$t/bin/id"
remote="$t/remote/Gabe-LS/rbstemsplus.git"
git init -q --bare -b main "$remote"
git clone -q "$remote" "$t/public" 2> "$t/clone.log"

dev="$t/dev"
git -C "$dev" init -q -b main
git -C "$dev" config user.name 'Export Test'
git -C "$dev" config user.email 'scan@example.invalid'
git -C "$dev" config rbstemsplus.publicEmail '12345678+Gabe-LS@users.noreply.github.com'
git -C "$dev" config remote.origin.url 'https://github.com/fixture-account/fixture-dev-repository'
cp "$rel" "$dev/scripts/release.sh"
cp "$root/scripts/public-scan.py" "$dev/scripts/"
# The scanner reads the committed allowlist. An excluded and an untracked match must be harmless.
printf '%s\t%s\n' hostname github.com \
  email '12345678+Gabe-LS@users.noreply.github.com' email 'git@github.com' > "$dev/.publicallow"
printf 'ops-fixture\n' > "$dev/.publicignore"
printf '%s\n' 1.5.0 > "$dev/VERSION"
printf 'See https://%s/Gabe-LS/rbstemsplus.\n' 'github.com' > "$dev/README.md"
printf '%s\n' '10.''199.72.109' > "$dev/ops-fixture/work.txt"
git -C "$dev" add -A
git -C "$dev" commit -qm 'Clean export fixture'
printf '%s\n' '10.''199.72.109' > "$dev/untracked.txt"
spoof_host='github.com''.invalid'
printf 'hostname\t%s\n' "$spoof_host" >> "$dev/.publicallow"
dry() { bash "$dev/scripts/release.sh" v1.5.0 --dry-run --public "$t/public" > "$t/log" 2>&1; }
die() { echo "public-scan-test: $*" >&2; exit 1; }
dry || die "clean export failed"
grep -qx 'public export privacy scan passed' "$t/log" || die "clean export missed the gate"

fixture_pc_user=fixturepcuser
git -C "$dev" config rbstemsplus.pcSsh "$fixture_pc_user@fixture-pc"
fixture_serial=FixtureDisk001
git -C "$dev" config rbstemsplus.pcT5Serial "$fixture_serial"
{
  printf 'LAN %s\n' '10.''199.72.109' '192.168.''249.110'
  printf 'Tailnet %s\n' '100.'{64.1.10,127.9.20,63.1.10,128.9.20}
  printf 'https://%s/\n' "$spoof_host"
  printf 'NAS_HOST="%s"\n' fixture-nas
  printf '/%s/%s/settings\n' Users fixturehome
  printf 'C:\\%s\\%s\\Settings\n' Users fixturewinuser
  printf 'Account %s\n' fixturepcuser
  printf 'Contact %s@%s\n' fixtureperson fixture-mail.invalid
  printf 'pass%s="%s"\n' word 'fixture-value-7!'
  printf 'SerialNumber="%s"\n' 'FixtureDisk001'
  printf '%s %s\n' Fixture Releaseperson
  printf '%s%s\n' fixture localuser
  printf 'https://%s/fixture-account/%s\n' github.com fixture-dev-repository
} > "$dev/planted.txt"
git -C "$dev" add -- planted.txt
git -C "$dev" commit -qm 'Plant synthetic private details'
if dry; then die "release accepted planted details"; fi
grep -qx 'release.sh: public export privacy scan failed' "$t/log" || die "wrong refusal"
sed -n '/^planted\.txt:/p' "$t/log" > "$t/actual"
cat > "$t/expected" <<'EOF'
planted.txt:1: [private-ip]
planted.txt:2: [private-ip]
planted.txt:3: [private-ip]
planted.txt:4: [private-ip]
planted.txt:7: [hostname]
planted.txt:8: [hostname]
planted.txt:9: [home-path]
planted.txt:10: [windows-account]
planted.txt:11: [windows-account]
planted.txt:12: [email]
planted.txt:13: [password-literal]
planted.txt:14: [device-serial]
planted.txt:15: [personal-name]
planted.txt:16: [local-username]
planted.txt:17: [private-repository]
EOF
cmp -s "$t/expected" "$t/actual" || die "missing, extra or unredacted finding"

stage="$t/content"
mkdir -p "$stage/nested"
python3 -I - "$stage" > "$t/content-allow" <<'PY'
from hashlib import sha256
from pathlib import Path
import struct
import sys
import zipfile
import zlib

stage = Path(sys.argv[1])
line = "LAN 10." "199.72.109\n"
(stage / "plain.png").write_text(line, encoding="utf-8")
for order in ("le", "be"):
    # Two non-ASCII code units and 18 ASCII code units exercise the 90% threshold.
    (stage / f"utf16-{order}.txt").write_bytes(("界界" + line).encode(f"utf-16-{order}"))

opaque = b"\0" + line.encode("ascii")
(stage / "nul.txt").write_bytes(opaque)
(stage / "zeros.txt").write_bytes(b"\0" * 20)
(stage / "allowed.bin").write_bytes(opaque)
(stage / "nested" / "copy.bin").write_bytes(opaque)
digest = sha256(opaque).hexdigest()
print(f"opaque\t{digest}\tallowed.bin")
print(f"opaque\t{digest}\tnested/")
print(f"opaque\t{digest}")

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

(stage / "metadata.png").write_bytes(
    b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 0, 0, 0, 0))
    + chunk(b"tEXt", b"Comment\0" + line.strip().encode("ascii"))
    + chunk(b"IDAT", zlib.compress(b"\0\0")) + chunk(b"IEND", b"")
)
with zipfile.ZipFile(stage / "archive.zip", "w") as archive:
    archive.writestr("note.txt", line)
PY
scan() {
  local rc=0
  python3 -I "$dev/scripts/public-scan.py" "$stage" "$1" "$dev" > "$t/actual" 2>&1 || rc=$?
  [ "$rc" -eq 1 ] || die "$2: expected scan exit 1, got $rc"
}
cat > "$t/expected" <<'EOF'
archive.zip:0: [opaque]
metadata.png:0: [opaque]
nested/copy.bin:0: [opaque]
nul.txt:0: [opaque]
plain.png:1: [private-ip]
utf16-be.txt:1: [private-ip]
utf16-le.txt:1: [private-ip]
zeros.txt:0: [opaque]
EOF
scan "$t/content-allow" 'planted content'
cmp -s "$t/expected" "$t/actual" || die "wrong findings for planted content"
printf 'changed' >> "$stage/allowed.bin"
{ printf '%s\n' 'allowed.bin:0: [opaque]'; cat "$t/expected"; } > "$t/changed-expected"
scan "$t/content-allow" 'changed opaque file'
cmp -s "$t/changed-expected" "$t/actual" || die "opaque exception accepted a changed file"

stage="$t/pem-export"
mkdir "$stage" "$t/pem"
pem="$t/pem"
openssl genrsa -traditional -out "$pem/rsa.pem" 2048 2>/dev/null ||
  openssl genrsa -out "$pem/rsa.pem" 2048 2>/dev/null
openssl ecparam -name prime256v1 -genkey -noout -out "$pem/ec.pem"
openssl pkcs8 -topk8 -nocrypt -in "$pem/ec.pem" -out "$pem/pkcs8.pem"
openssl pkcs8 -topk8 -in "$pem/ec.pem" -passout pass:fixture -out "$pem/encrypted.pem"
openssl ec -in "$pem/ec.pem" -aes256 -passout pass:fixture -out "$pem/legacy.pem" 2>/dev/null
openssl dsaparam -out "$pem/dsa-params.pem" 1024 2>/dev/null
openssl gendsa -out "$pem/dsa-generated.pem" "$pem/dsa-params.pem" 2>/dev/null
openssl dsa -in "$pem/dsa-generated.pem" -out "$pem/dsa.pem" 2>/dev/null
ssh-keygen -q -t ed25519 -N '' -C fixture -f "$pem/openssh.pem"
openssl pkey -in "$pem/ec.pem" -pubout -out "$stage/public.pem"
printf '%s\n' 'The -----BEGIN PRIVATE KEY----- marker' '-----BEGIN RSA PRIVATE KEY-----' \
  'Short example:' '-----BEGIN EC PRIVATE KEY-----\nMIIE\n-----END EC PRIVATE KEY-----' \
  > "$stage/markers.txt"
printf '%s\n' '"-----BEGIN PRIVATE KEY-----\n" +' \
  '"This line of prose has enough letters to exceed the minimum body length.\n" +' \
  '"-----END PRIVATE KEY-----\n";' > "$stage/prose.js"
printf '%s\n' 'PuTTY-User-Key-File-3: ssh-ed25519' 'Private-Lines: 2' 'sample' \
  > "$stage/truncated.ppk"
python3 -I "$dev/scripts/public-scan.py" "$stage" "$dev/.publicallow" "$dev" > "$t/actual" 2>&1 \
  || die "public key or harmless marker refused"

# Do not put generated keys in Git, even in the temporary fixture repository.
python3 -I - "$pem" "$stage" <<'PY'
import json
from base64 import b64encode
from os import urandom
from pathlib import Path
import sys

pem, stage = map(Path, sys.argv[1:])
labels = {"rsa": "RSA ", "ec": "EC ", "dsa": "DSA ", "pkcs8": "",
          "encrypted": "ENCRYPTED ", "legacy": "EC ", "openssh": "OPENSSH "}
keys = {}
for name, label in labels.items():
    key = (pem / f"{name}.pem").read_text(encoding="ascii")
    if not key.startswith(f"-----BEGIN {label}PRIVATE KEY-----\n"):
        sys.exit(f"wrong PEM fixture format for {name}")
    keys[name] = key

(stage / "rsa.md").write_text("# Example\n\n```pem\n" + keys["rsa"] + "```\n", encoding="ascii")
crlf = "Example\r\n" + "".join(f"\t  {line} \t\r\n" for line in keys["ec"].splitlines())
(stage / "ec.txt").write_bytes(crlf.encode("ascii"))
(stage / "escaped.json").write_text("{\n  \"key\": " + json.dumps(crlf) + "\n}\n", encoding="ascii")
(stage / "openssh.json").write_text(json.dumps(keys["openssh"]).replace("/", r"\/"), encoding="ascii")
for name in ("dsa", "pkcs8", "encrypted", "legacy"):
    (stage / f"{name}.txt").write_text(keys[name], encoding="ascii")
(stage / "generic.pem").write_text(keys["pkcs8"].replace("PRIVATE KEY", "ED25519 PRIVATE KEY"), encoding="ascii")
for name, prefix, wrapper, between, ending in [
    ("adjacent.c", "char key[] = ", "", "", ";"),
    ("concatenated.js", "const key = ", "", " +", ";"),
    ("list.py", "key = [", "", ",", "]"),
    ("continued.c", "#define KEY ", "", " \\", ""),
    ("parenthesized.js", "const key = ", "(", ") +", ");"),
]:
    literals = [wrapper + json.dumps(line) for line in keys["pkcs8"].splitlines(keepends=True)]
    (stage / name).write_text(prefix + (between + "\n").join(literals) + ending + "\n", encoding="ascii")
(stage / "inline.js").write_text(
    "const key = " + " + ".join(map(json.dumps, keys["pkcs8"].splitlines(keepends=True))) + ";\n", encoding="ascii",
)
(stage / "triple.py").write_text('key = """' + keys["pkcs8"].rstrip() + '"""\n', encoding="ascii")
(stage / "triple.swift").write_text('let key = """\n' + keys["pkcs8"] + '"""\n', encoding="ascii")

def random_line():
    return b64encode(urandom(48)).decode("ascii")

for version in (2, 3):
    (stage / f"putty-{version}.ppk").write_text(
        f"PuTTY-User-Key-File-{version}: ssh-ed25519\nEncryption: none\nComment: generated\n"
        f"Public-Lines: 1\n{random_line()}\nPrivate-Lines: 2\n{random_line()}\n{random_line()}\n"
        "Private-MAC: " + urandom(20 if version == 2 else 32).hex() + "\n",
        encoding="ascii",
    )
truncated = keys["pkcs8"].splitlines()
(stage / "truncated.md").write_text("Pasted:\n" + "\n".join(truncated[:-1]) + "\n(cut off)\n", encoding="ascii")
(stage / "trailing.md").write_text(
    "\n".join(truncated[:-1] + [truncated[-1] + " (from the old server)"]) + "\n", encoding="ascii",
)
(stage / "hex.txt").write_text(
    "-----BEGIN PRIVATE KEY-----\n" + "0123456789abcdef" * 4 + "  \n-----END PRIVATE KEY-----\n",
    encoding="ascii",
)
PY
cat > "$t/expected" <<'EOF'
adjacent.c:1: [private-key]
concatenated.js:1: [private-key]
continued.c:1: [private-key]
dsa.txt:1: [private-key]
ec.txt:2: [private-key]
encrypted.txt:1: [private-key]
escaped.json:2: [private-key]
generic.pem:1: [private-key]
hex.txt:1: [private-key]
inline.js:1: [private-key]
legacy.txt:1: [private-key]
list.py:1: [private-key]
openssh.json:1: [private-key]
parenthesized.js:1: [private-key]
pkcs8.txt:1: [private-key]
putty-2.ppk:1: [private-key]
putty-3.ppk:1: [private-key]
rsa.md:4: [private-key]
trailing.md:1: [private-key]
triple.py:1: [private-key]
triple.swift:2: [private-key]
truncated.md:2: [private-key]
EOF
if command -v gpg >/dev/null 2>&1; then
  (
    # Keep the agent socket paths below the Unix socket path length limit.
    GNUPGHOME="$(mktemp -d /tmp/rbsp-gpg.XXXXXX)"
    export GNUPGHOME
    trap 'gpgconf --kill gpg-agent >/dev/null 2>&1 || true; rm -rf "$GNUPGHOME"' EXIT
    gpg --batch --passphrase '' --pinentry-mode loopback --quick-gen-key 'Public Scan Fixture' ed25519 sign 0 \
      >/dev/null 2>&1 || die "could not generate GPG fixture"
    gpg --batch --passphrase '' --pinentry-mode loopback --emit-version --comment generated \
      --export-secret-keys --armor > "$stage/pgp.asc" 2>/dev/null || die "could not export GPG fixture"
  )
  printf '%s\n' 'pgp.asc:1: [private-key]' >> "$t/expected"
  LC_ALL=C sort -o "$t/expected" "$t/expected"
else
  echo "public-scan-test: skipping PGP case (gpg missing)"
fi
scan "$dev/.publicallow" 'planted private keys'
cmp -s "$t/expected" "$t/actual" || die "missing, extra or unredacted private-key findings"

stage="$t/origin"
mkdir "$stage"
printf 'Repository fixture-account/%s\n' fixture-dev-repository > "$stage/remote.txt"
printf '%s\n' 'remote.txt:1: [private-repository]' > "$t/expected"
for prefix in 'ssh:''//git@github.com/' 'git@github.com:'; do
  git -C "$dev" config remote.origin.url "${prefix}fixture-account/fixture-dev-repository.git"
  scan "$dev/.publicallow" 'GitHub SSH origin'
  cmp -s "$t/expected" "$t/actual" || die "wrong finding for GitHub SSH origin"
done
echo "public-scan-test: clean export passed, planted content, private keys and SSH origins checked with redacted findings"
