#!/usr/bin/env python3
"""The Windows carry-forward's read (release.md §1.3, T13), for scripts/windows-assets.sh:

    win-carry.py prev PAYLOAD   the previous release's windows block, from its payload.json
                                (already verified by the caller), as shell assignments:
                                prev_win_app_version, prev_win_zip, prev_win_zip_sha256,
                                prev_win_admin_sha256, prev_win_stub_x64_sha256,
                                prev_win_stub_arm64_sha256, prev_win_bridge_file,
                                prev_win_bridge_sha256. Exit 3: no windows block (nothing to
                                carry); 1: a field isn't what a windows block holds."""
import json
import re
import sys

VER = re.compile(r"\d+\.\d+\.\d+")
SHA = re.compile(r"[0-9a-f]{64}")


def need(v, rx, what):
    if not isinstance(v, str) or not rx.fullmatch(v):
        sys.exit("%s is %r" % (what, v))
    return v


def prev(path):
    d = json.load(open(path, encoding="utf-8"))
    if not isinstance(d, dict) or "windows" not in d:
        return 3
    w = d["windows"]
    if not isinstance(w, dict):
        sys.exit("windows isn't an object")
    app, bridge = w.get("app") or {}, w.get("bridge") or {}
    stub = app.get("stub_sha256") or {}
    out = {
        "prev_win_app_version": need(app.get("version"), VER, "windows.app.version"),
        "prev_win_zip": need(app.get("zip"), re.compile(r"rbstemsplus-windows\.zip"), "windows.app.zip"),
        "prev_win_zip_sha256": need(app.get("sha256"), SHA, "windows.app.sha256"),
        "prev_win_admin_sha256": need(app.get("admin_sha256"), SHA, "windows.app.admin_sha256"),
        "prev_win_stub_x64_sha256": need(stub.get("x64"), SHA, "windows.app.stub_sha256.x64"),
        "prev_win_stub_arm64_sha256": need(stub.get("arm64"), SHA, "windows.app.stub_sha256.arm64"),
        "prev_win_bridge_file": need(bridge.get("file"), re.compile(r"rbstemsplus-bridge-win-x64\.dll"), "windows.bridge.file"),
        "prev_win_bridge_sha256": need(bridge.get("sha256"), SHA, "windows.bridge.sha256"),
    }
    for k, v in out.items():
        print("%s=%s" % (k, v))   # every value matched a plain pattern above: no quoting needed
    return 0


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "prev":
        sys.exit(prev(sys.argv[2]))
    sys.exit("usage: win-carry.py prev PAYLOAD")
