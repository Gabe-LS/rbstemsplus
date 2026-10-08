#!/bin/bash
# Makes RB Stems Plus's release signing keys (docs/RELEASING.md, "Signing keys"). Run it
# yourself, once, in a NEW Terminal window, and quit Terminal afterwards with Option-Command-Q
# (Quit and Close All Windows): Terminal saves its windows' contents to reopen them, so the backup
# key shown here could otherwise stay on disk in the saved window state.
#
#   scripts/make-signing-keys.sh                  both keys
#   scripts/make-signing-keys.sh --release-only   only a new everyday key, when the old one is
#                                                 lost (the release that adds it is signed with
#                                                 the backup key)
#
# Two ECDSA P-256 keys; the app and the bootstrap accept a payload.json signed by either:
#   - the everyday release key, which scripts/release.sh signs each release with. Its private
#     key is saved encrypted (PKCS#8, AES-256, a passphrase you choose, which openssl asks for
#     itself: never on a command line, in the environment or in a file) in
#     ~/.rbstemsplus-signing/release-key.pem (folder 700, file 600). macOS's openssl (LibreSSL)
#     derives the AES key with PBKDF2-SHA1 and only 2048 iterations, so the passphrase carries
#     the strength: use a long random one from the password manager (e.g. 6 or more random words,
#     or 20+ random characters);
#   - the backup key, for when the everyday key is lost. Its private key is shown on the screen
#     once, for your password manager, then cleared: it is never written to disk. You paste it
#     back once to check the copy.
# Both public keys go to keys/release.pub.pem and keys/backup.pub.pem: commit them. Nothing is
# overwritten: an existing key stops it.
#
# Only macOS's own /usr/bin/openssl is used. The private keys exist only in this script's memory
# and in pipes to openssl, never in a command's arguments.
set -euo pipefail
umask 077
root="$(cd "$(dirname "$0")/.." && pwd)"
OPENSSL=/usr/bin/openssl
dir="$HOME/.rbstemsplus-signing"
private="$dir/release-key.pem"
keys="$root/keys"
release_pub="$keys/release.pub.pem"
backup_pub="$keys/backup.pub.pem"

die() { echo "make-signing-keys.sh: $*" >&2; exit 1; }
# Clears the screen and Terminal's scrollback.
wipe() { printf '\033[H\033[2J\033[3J'; }

release_only=0
case "${1:-}" in
  "") ;;
  --release-only) release_only=1 ;;
  *) echo "usage: $0 [--release-only]" >&2; exit 2 ;;
esac
[ -t 0 ] && [ -t 1 ] || die "run it yourself in Terminal: it shows a key and asks for a passphrase"
[ -x "$OPENSSL" ] || die "$OPENSSL is missing"

# nothing is ever overwritten
[ ! -e "$private" ] && [ ! -L "$private" ] \
  || die "$private exists already. If that key is lost or replaced, move the file aside first."
[ ! -e "$release_pub" ] || die "keys/release.pub.pem exists already. For a new everyday key: git rm keys/release.pub.pem, then run this with --release-only."
if [ "$release_only" = 1 ]; then
  [ -f "$backup_pub" ] || die "--release-only needs the backup key's keys/backup.pub.pem"
else
  [ ! -e "$backup_pub" ] || die "keys/backup.pub.pem exists already."
fi
if [ -e "$dir" ] || [ -L "$dir" ]; then
  [ -d "$dir" ] && [ ! -L "$dir" ] && [ -O "$dir" ] || die "$dir isn't a folder of this account's"
else
  mkdir -m 700 "$dir"
fi
chmod 700 "$dir"
mkdir -p -m 755 "$keys"

tmp="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-keys.XXXXXX")"      # public keys and the encrypted key only
trap 'rm -rf "${tmp:?}"' EXIT

wipe
cat <<'EOF'
RB Stems Plus: making the release signing keys

Before you go on:
  - This must be a NEW Terminal window, with nothing else in it. When this script has finished,
    quit Terminal with Option-Command-Q (Quit and Close All Windows), so it doesn't reopen this
    window next time: Terminal saves its windows' contents for that, and the backup key shown
    here could stay in that saved state on disk.
  - The everyday key's passphrase: make it long and random, from your password manager (6 or
    more random words, or 20+ random characters). macOS's openssl protects the saved key with
    only 2048 rounds of PBKDF2-SHA1, so the passphrase is what keeps it safe.

EOF
read -r -p "Press Return to go on, or Control-C to stop (no key has been made yet). " _

# ------------------------------------------------------------------ the backup key

if [ "$release_only" = 0 ]; then
  backup="$("$OPENSSL" ecparam -name prime256v1 -genkey -noout)" || die "couldn't make the backup key"
  printf '%s\n' "$backup" | "$OPENSSL" ec -pubout -out "$tmp/backup.pub.pem" 2>/dev/null || die "couldn't read the backup key"
  while true; do
    wipe
    cat <<'EOF'
RB Stems Plus: the BACKUP signing key

Save the private key below in your password manager now, as a secure note named
"RB Stems Plus backup signing key". Copy every line, from -----BEGIN to -----END, included.

It is shown only this once and is not saved anywhere on this Mac. Without it, losing the
everyday key means the installed apps can't be updated any more.

EOF
    printf '%s\n\n' "$backup"
    read -r -p "When it is saved, press Return (the screen is then cleared). " _
    wipe
    echo "To check the copy, paste the backup key from your password manager, then press Return."
    echo "It isn't shown."
    pasted="" line=""
    # password managers may paste it with other line breaks (\r, spaces) or on one line: read until
    # the END marker or an empty line, and compare without any whitespace
    while IFS= read -rs line; do
      line="$(printf '%s' "$line" | tr -d ' \t\r')"
      [ -n "$line" ] || { [ -n "$pasted" ] && break; continue; }
      pasted="$pasted$line"
      case "$line" in *"-----END"*"-----") break ;; esac
    done
    while IFS= read -rs -t 1 line; do :; done      # the Return pressed after the paste, and anything after it
    echo
    if [ "$pasted" = "$(printf '%s' "$backup" | tr -d ' \t\r\n')" ]; then pasted=""; echo "The copy matches."; break; fi
    pasted=""
    read -r -p "That isn't the same key. Press Return to see it again. " _
  done
  backup=""
fi

# ------------------------------------------------------------------ the everyday key

echo
echo "The everyday release key: choose a long random passphrase from your password manager"
echo "(it protects the key on this Mac; scripts/release.sh asks for it at each release)."
echo "openssl asks for it twice."
plain="$("$OPENSSL" ecparam -name prime256v1 -genkey -noout)" || die "couldn't make the everyday key"
printf '%s\n' "$plain" | "$OPENSSL" ec -pubout -out "$tmp/release.pub.pem" 2>/dev/null || die "couldn't read the everyday key"
tries=0
until printf '%s\n' "$plain" | "$OPENSSL" pkcs8 -topk8 -v2 aes-256-cbc -out "$tmp/release-key.pem"; do
  tries=$((tries + 1))
  [ "$tries" -lt 3 ] || { plain=""; die "the passphrase wasn't set; nothing was saved"; }
  echo "Try again."
done
plain=""
tries=0
echo "Check: type the passphrase once more."
until "$OPENSSL" ec -in "$tmp/release-key.pem" -pubout 2>/dev/null | cmp -s - "$tmp/release.pub.pem"; do
  tries=$((tries + 1))
  [ "$tries" -lt 5 ] || die "the passphrase didn't open the key; nothing was saved"
  echo "That didn't open it. Try again."
done

# ------------------------------------------------------------------ saving

if [ "$release_only" = 0 ] && cmp -s "$tmp/release.pub.pem" "$tmp/backup.pub.pem"; then
  die "the two keys are the same (that can't happen): nothing was saved"
fi
if [ "$release_only" = 1 ] && cmp -s "$tmp/release.pub.pem" "$backup_pub"; then
  die "the new everyday key is the backup key (that can't happen): nothing was saved"
fi
mv -n "$tmp/release-key.pem" "$private" && [ ! -e "$tmp/release-key.pem" ] || die "couldn't save $private"
chmod 600 "$private"
cp -n "$tmp/release.pub.pem" "$release_pub"
[ "$release_only" = 1 ] || cp -n "$tmp/backup.pub.pem" "$backup_pub"
chmod 644 "$keys"/*.pub.pem

echo
echo "Done."
echo "  everyday key (encrypted): $private"
[ "$release_only" = 1 ] || echo "  backup key:               only in your password manager"
echo "  public keys:              keys/release.pub.pem$([ "$release_only" = 1 ] || echo ", keys/backup.pub.pem")"
echo
echo "Commit the public keys (git add keys), then build and release as docs/RELEASING.md says."
echo "Keep the passphrase somewhere safe too (your password manager): without it the everyday key"
echo "can't sign."
echo
echo "Now quit Terminal with Option-Command-Q (Quit and Close All Windows), so it doesn't reopen"
echo "this window: Terminal keeps its windows' contents for that."
