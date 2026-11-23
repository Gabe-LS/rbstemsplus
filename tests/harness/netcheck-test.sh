#!/bin/bash
# The local probes, with every command that could reach the network replaced.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
netcheck="${1:-$here/netcheck.sh}"
t="$(mktemp -d "${TMPDIR:-/tmp}/rbsp-netcheck-test.XXXXXX")"
trap 'rm -rf "$t"' EXIT
export NETCHECK_TSV="$t/rows" PROBES="$t/probes"
export PATH="$t:$PATH"
cat > "$t/curl" <<'EOF'
#!/bin/bash
printf '%s\n' "${@: -1}" >> "$PROBES"
printf 200
EOF
cat > "$t/git" <<'EOF'
#!/bin/bash
case "$3 ${4:-} ${5:-}" in
  'config --get rbstemsplus.ntfyHost') [ -n "${TEST_NTFY:-}" ] && printf '%s\n' "$TEST_NTFY" ;;
  'config --get rbstemsplus.nasHost') [ -n "${TEST_NAS:-}" ] && printf '%s\n' "$TEST_NAS" ;;
  'ls-remote --heads origin') exit 0 ;;
  *) exit 1 ;;
esac
EOF
printf '#!/bin/sh\nexit 0\n' > "$t/gh"
chmod +x "$t/curl" "$t/git" "$t/gh"

unset TEST_NTFY TEST_NAS
"$netcheck" --no-ai --no-pc > "$t/log"
grep -q $'^SKIP\tcurl\tntfyHost\t' "$NETCHECK_TSV"
grep -q $'^SKIP\tcurl\tnasHost\t' "$NETCHECK_TSV"
public_probes="$(wc -l < "$PROBES")"

: > "$PROBES"
: > "$NETCHECK_TSV"
export TEST_NTFY="fixture-notify" TEST_NAS="fixture-nas"
"$netcheck" --no-ai --no-pc > "$t/log"
grep -qxF "https://$TEST_NTFY/" "$PROBES"
grep -qxF "http://$TEST_NAS:8090/" "$PROBES"
[ "$(wc -l < "$PROBES")" -eq "$((public_probes + 2))" ]
grep -q $'^OK\tcurl\tfixture-notify\t' "$NETCHECK_TSV"
grep -q $'^OK\tcurl\tfixture-nas:8090 (ntfy, home LAN)\t' "$NETCHECK_TSV"
echo "netcheck-test: unset probes skip and configured probes use the local hosts"
