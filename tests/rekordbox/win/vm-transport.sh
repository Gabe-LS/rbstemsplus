# Sourced by pc-session.sh for the kit's Parallels VM ($vm_name). The VM runner owns target
# validation, scheduling and timeouts. The kit runs as the VM's signed-in user ($test_user);
# admin work runs as SYSTEM.
vm_share="${VMRUN_SHARE:-$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/../build/win}"
vm_quote() { printf "'%s'" "${1//\'/\'\'}"; }
vm_run() {
  VMRUN_VM="$vm_name" VMRUN_UUID= VMRUN_RBKIT_USER="$test_user" VMRUN_SHARE="$vm_share" "$root/tests/win/vm-run.sh" "$@"
}

# Parallels' high resolution off for the VM (the guest then runs at 100 %): prints on (it was on,
# now off) or off; never for the owner's VM. Works while the VM runs; the scale follows at sign-in
vm_hires_off() {
  local prl="${VMRUN_PRLCTL:-prlctl}" info
  [ "$vm_name" != 'Windows 11' ] || return 1
  info="$(within 60 "$prl" list -i "$vm_name")" || return 1
  [ "$(sed -n 's/^Name: //p' <<< "$info")" = "$vm_name" ] || return 1
  if grep -Eq '^[[:space:]]*video .* high-resolution=on( |$)' <<< "$info"; then
    within 60 "$prl" set "$vm_name" --high-resolution off > /dev/null || return 1
    echo on
  else echo off; fi
}

vm_ps() {   # ROLE SECS SCRIPT [IDLE]
  local mode=sys rc out line idle="${4:-0}"
  [ "$1" != "$test_login" ] || mode=user-signed
  out="$(printf '%s\n' "$3" | vm_run "$mode" "$2" --rbkit "$idle" | tr -d '\r'; exit "${PIPESTATUS[1]}")"; rc=$?
  # the run's id names a VM other than rb-win-template (vm-run.sh's VMTAG)
  local marker='^vm-run-exit one-[0-9]{8}-[0-9]{6}(-[A-Za-z0-9._-]+)?-[0-9]+ 1 '
  if [ -n "$out" ]; then
    while IFS= read -r line; do
      case "$line" in
        'vm-run-exit one-'*) ;;
        'vm-run:'* | 'vm-run.sh:'* | *" prlctl didn't start the job: again ("*) printf '%s\n' "$line" >&2 ;;
        *) printf '%s\n' "$line" ;;
      esac
    done <<< "$out"
  fi
  case "$rc" in 124 | 142) return 142 ;; esac
  grep -Eq "${marker}$rc\$" <<< "$out" || return 255
  return "$rc"
}

vm_transfer() (   # put|get SECS LOGIN DEST PATH...; short-lived files in the existing \\Mac\win share
  local action="$1" secs="$2" login="$3" dest="$4" dir unc script path leaf parent archive n=0
  shift 4
  dir="$(mktemp -d "$vm_share/rbkit-transfer.XXXXXX")" || return 1
  trap 'rm -rf "$dir"' EXIT
  unc="\\\\Mac\\win\\${dir##*/}"
  script="\$ErrorActionPreference = 'Stop'"$'\n'
  for path; do
    case "$action" in
      put)
        leaf="${path##*/}"
        cp -R "$path" "$dir/" || return 1
        script+="Copy-Item -LiteralPath $(vm_quote "$unc\\$leaf") -Destination $(vm_quote "$dest") -Recurse -Force"$'\n'
        # files copied from \\Mac\win arrive read-only (scp's don't); the kit later replaces some
        script+="Get-Item -LiteralPath (Join-Path $(vm_quote "$dest") $(vm_quote "$leaf")) -Force | ForEach-Object { if (\$_.PSIsContainer) { Get-ChildItem -LiteralPath \$_.FullName -Recurse -Force -File } else { \$_ } } | ForEach-Object { \$_.IsReadOnly = \$false }"$'\n' ;;
      get)
        parent="${path%/*}"; leaf="${path##*/}"; n=$((n + 1))
        [[ "$parent" != [A-Za-z]: ]] || parent+='/'; parent="${parent//\//\\}"
        # One archive per source. All guest-created files are direct children of the host-owned
        # directory, so the host can unlink them even when Parallels writes them as root.
        script+="& tar.exe -cf $(vm_quote "$unc\\$n.tar") -C $(vm_quote "$parent") $(vm_quote "./$leaf"); if (\$LASTEXITCODE -ne 0) { exit 1 }"$'\n' ;;
    esac
  done
  vm_ps "$login" "$secs" "$script" || return
  if [ "$action" = get ]; then
    for archive in "$dir/"*.tar; do tar -xf "$archive" -C "$dest" || return 1; done
  fi
)
