"""Exercise T16's CLI contract with test audio and the real kept/manifest code."""

import json
import os
from pathlib import Path
import subprocess
import sys
import time


helper = Path(sys.argv[1])
scratch = Path(sys.argv[2]) / "cli"
scratch.mkdir()
cache, kept = scratch / "cache", scratch / "kept"
runner = Path(__file__).with_name("kept-run.py")
track, other, missing = (scratch / name for name in ("track a.wav", "track b.wav", "absent.wav"))
track.write_bytes(b"a")
other.write_bytes(b"b")
set_path = kept / "case.kept"
manifest = kept / "case.manifest"
passed = 0
drive_paths = ["--rekordbox", r"C:\rekordbox", "--ort", "d:/runtime.dll"]


def check(condition, description):
    global passed
    assert condition, description
    passed += 1
    print(f"ok   {description}")


def invoke(*args, target=None, stop=None, code=0, **settings):
    command = [helper, "--model", "fixture", "--cache", target or cache, "--kept-dir", kept, *drive_paths, *args]
    if stop is not None:
        command = [sys.executable, "-I", runner, str(stop), *command]
    env = dict(os.environ, RBSP_TEST_STEMS="all", **settings)
    proc = subprocess.run(command, env=env, capture_output=True, timeout=10)
    assert proc.returncode == code, (command, proc.returncode, proc.stderr, proc.stdout)
    assert b"\r" not in proc.stdout, proc.stdout
    return [json.loads(row) for row in proc.stdout.splitlines()]


def rows():
    return set(set_path.read_bytes().splitlines()[2:])


before = time.time()
out = invoke("--keep", "case", track, other)
initial = rows()
expiry = out[-1]["until"]
check(len(initial) == 4 and abs(expiry - before - 864000) < 5 and
      set_path.read_bytes().splitlines()[1] == f"until {expiry}".encode() and
      b"\r" not in manifest.read_bytes(), "a completed run writes both tracks and reports the default expiry")

# Split the path across components so the fixture exceeds MAX_PATH, while each component fits.
parent = scratch / ("p" * 100)
long_kept = parent / ("k" * (300 - len(os.fsencode(parent)) - 1))
assert len(os.fsencode(long_kept)) == 300
invoke("--kept-dir", long_kept, "--keep", "case", track, other)
check((long_kept / "case.kept").is_file() and (long_kept / "case.manifest").is_file(),
      "a 300-byte --kept-dir publishes the named set and manifest at the full path")

for option in ("--rekordbox", "--ort"):
    for path in ("", "relative", "C:relative", r"\rooted", r"\\server\share", r"\\?\C:\extended"):
        command = [helper, "--model", "fixture", "--cache", cache, *drive_paths, option, path, track]
        proc = subprocess.run(command, env=dict(os.environ, RBSP_TEST_NO_MODEL="1"), capture_output=True, timeout=10)
        check(proc.returncode == 2 and not proc.stdout and option.encode() in proc.stderr,
              f"PF2(b): run refuses {option} {path!r} before loading")

invalid_paths = ("--rekordbox", "", "--ort", "C:relative")
for mode in ("--estimate", "--quick"):
    out = invoke(mode, *invalid_paths, track, RBSP_TEST_NO_MODEL="1")
    check(out[-1]["estimate"] == 1, f"PF2(b): {mode} accepts unused paths")
list_args = (*invalid_paths, "--library", "pf2-fixture")
check(invoke("--playlists", *list_args, RBSP_TEST_NO_MODEL="1") == [{}], "PF2(b): --playlists accepts unused paths")
check(invoke("--tracks", "pf2", *list_args, RBSP_TEST_NO_MODEL="1") == [{"file": r"C:\fixture.wav"}],
      "PF2(b): --tracks accepts unused paths")

out = invoke("--keep", "case", "--no-keep", track)
check("kept" not in out[-1] and rows() == initial, "--no-keep leaves the prior set alone")
invoke("--keep", "bad name", track, code=1)
invoke("--keep", "case", "--until", str(int(before) - 1), track, code=2)
check(rows() == initial, "an invalid name or past expiry leaves the existing set intact")

chosen = scratch / "moved cache"
cache.rename(chosen)
snapshot = (set_path.read_bytes(), manifest.read_bytes())
invoke("--keep", "case", "--chosen", track, code=1)
check(not cache.exists() and snapshot == (set_path.read_bytes(), manifest.read_bytes()),
      "a moved chosen cache is refused without rewriting the kept files")
out = invoke("--keep", "case", "--chosen", track, other, target=chosen)
check(rows() == initial and sum(row.get("already", 0) for row in out) == 4 and
      all(row.get("how") == "unchanged since it was prepared" for row in out[:-1]),
      "the relocated cache reuses both tracks")
chosen.rename(cache)

out = invoke("--keep", "case", "--first", "1", track, "streaming:id", other)
check(out[-1]["done"] == 1 and rows() == initial and str(other).encode() in manifest.read_bytes(),
      "--first preserves the track beyond N")
out = invoke("--keep", "case", "--first", "1", track, missing)
check(len(rows()) == 2 and str(other).encode() not in manifest.read_bytes(),
      "--first releases a track that left the selection")

saved = manifest.read_bytes()
track.rename(scratch / "unplugged.wav")
out = invoke("--keep", "case", track)
check(out[0]["result"] == "skipped" and len(rows()) == 2 and b" 1 2 " in manifest.read_bytes(),
      "a skipped track retains its completed record")
(scratch / "unplugged.wav").rename(track)

for path in ("E:/missing/a.wav", r"e:\missing\a.wav", "//server/share/a.wav"):
    manifest.write_bytes(saved.replace(str(track).encode(), path.encode()))
    out = invoke("--quick", "--keep", "case", path, "streaming:id")
    check(out[0] == {"file": path, "result": "unchecked"} and len(out) == 2 and
          out[-1]["estimate"] == out[-1]["unchecked"] == 1 and
          sum(out[-1][k] for k in ("ready", "prepare", "skipped", "unchecked")) == 1,
          f"quick estimate accepts {path} and reconciles the unchecked total")
out = invoke("--quick", "--keep", "case", target="E:/unmounted/cache")
check(out[0]["result"] == "unchecked" and out[-1]["cache_gb"] == 0,
      "quick estimate leaves an absent off-drive cache unopened")
manifest.write_bytes(saved)
out = invoke("--estimate", "--keep", "case", track, missing, "streaming:id")
check([r["result"] for r in out[:-1]] == ["ready", "skipped"] and
      out[-1]["estimate"] == sum(out[-1][k] for k in ("ready", "prepare", "skipped", "unchecked")) == 2,
      "a completed estimate accounts for each accepted path")

listing = scratch / "keys.txt"
listing.write_bytes(os.fsencode(track))
key_args = [helper, "--keys", "--model", "fixture"]
bad_path = subprocess.run([*key_args, "--rekordbox", "C:relative", "--list", scratch / "absent.txt"],
                          capture_output=True, timeout=10)
check(bad_path.returncode == 2 and not bad_path.stdout and
      bad_path.stderr == b"--rekordbox needs a drive-absolute path\n",
      "research mode refuses a drive-relative rekordbox path before reading the list")
key_args += ["--rekordbox", r"C:\rekordbox"]
key_run = subprocess.run([*key_args, "--list", listing],
                         cwd=scratch, env=dict(os.environ, RBSP_TEST_STEMS="all", RBSP_TEST_NO_MODEL="1",
                                               RBSP_TEST_BINARY_LIST="1"),
                         capture_output=True, check=True, timeout=10)
keys = {line.split(b"\t")[1] for line in key_run.stdout.splitlines() if line.startswith(b"key\t")}
check(keys == {line.split()[1] for line in rows()}, "--keys reads a binary list and agrees with the kept rows")

crlf = scratch / "keys-crlf.txt"
crlf.write_bytes(os.fsencode(track) + b"\r\n")
crlf_run = subprocess.run([*key_args, "--list", crlf],
                          cwd=scratch, env=dict(os.environ, RBSP_TEST_STEMS="all", RBSP_TEST_NO_MODEL="1",
                                                RBSP_TEST_BINARY_LIST="1"),
                          capture_output=True, timeout=10)
crlf_keys = {line.split(b"\t")[1] for line in crlf_run.stdout.splitlines() if line.startswith(b"key\t")}
check(crlf_run.returncode == 0 and crlf_keys == keys, "--keys reads a CRLF list (the Windows app's line endings) as the same paths")

entry_a, entry_b = sorted(cache.rglob("*.flac"))[:2]
for entry, tag in ((entry_a, b"1"), (entry_b, b"w0")):
    data = entry.read_bytes()
    offset = data.index(b"RBSTEMS_HELPER=")
    comment = b"RBSTEMS_HELPER=" + tag
    entry.write_bytes(data[:offset - 4] + len(comment).to_bytes(4, "little") + comment)
manifest.unlink()                      # exercise the FLAC reuse check, past the manifest fast path
out = invoke("--no-keep", track)
manifest.write_bytes(saved)
check(out[-2]["written"] == out[-2]["already"] == 1 and
      entry_a.read_bytes().endswith(b"RBSTEMS_HELPER=1") and entry_b.read_bytes().endswith(b"RBSTEMS_HELPER=w1"),
      "the Windows tag rewrites an older Windows entry and retains a Mac entry")

with entry_a.open("ab") as f:
    f.write(b"x" * (4 * 1024**2))
new_track = scratch / "new.wav"
new_track.write_bytes(b"c")
out = invoke("--keep", "extra", "--max-gb", "0.008", new_track)
check("75%" in out[-1]["stopped"] and out[-2]["written"] == 0,
      "the run refuses new kept entries that exceed the combined share")

before_stop = rows()
out = invoke("--keep", "case", new_track, track, stop=0, RBSP_TEST_STOP="1")
check(out[-1]["stopped"] == "stopped" and out[-1]["done"] == 1 and
      len(rows() - before_stop) == 1 and before_stop < rows() and b" 0 1 " in manifest.read_bytes(),
      "EOF after chunk progress leaves both the old set and the newly written partial record")

errors = scratch / "runner.errors"
env = dict(os.environ, RBSP_TEST_RUN_ERRORS=str(errors))
bad = subprocess.run([sys.executable, "-I", runner, "-1", sys.executable, "-c",
                      "import sys; sys.stdout.buffer.write(b'{}\\r\\n')"], env=env, capture_output=True, timeout=10)
check(bad.returncode == 125 and "contains CR" in errors.read_text(), "the driver detects CR before shell capture")
errors.unlink()
bad = subprocess.run([sys.executable, "-I", runner, "2", sys.executable, "-c", "pass"],
                     env=env, capture_output=True, timeout=10)
check(bad.returncode == 125 and "never reached chunk 2" in errors.read_text(),
      "the driver fails when the helper exits before the stop milestone")
# The source metadata and the old cache entries survive a Windows decoder update.
aac, later, alac = (scratch / name for name in ("first.mp4", "later.mp4", "lossless.m4a"))
for path, marker in zip((aac, later, alac), (3, 4, 5)):
    path.write_bytes(bytes([marker]))
old_mf = {"RBSP_TEST_AAC_DECODER": "10.0.1.0"}
new_mf = {"RBSP_TEST_AAC_DECODER": "10.0.2.0", "RBSP_TEST_AAC_NEW_PCM": "1"}
invoke("--keep", "case", aac, later, alac, **old_mf)
old_manifest = manifest.read_bytes()
old_keys = rows()

def results(mode, *paths, **settings):
    return [row["result"] for row in invoke(mode, *paths, **settings) if "file" in row]


def rerun(**settings):
    return next(row for row in invoke("--keep", "case", "--first", "1", aac, later, alac, **settings)
                if row.get("file") == str(aac))


same_run = rerun(**old_mf)
check(results("--quick", aac, later, alac, **old_mf) == ["ready"] * 3 and
      results("--estimate", aac, alac, **old_mf) == ["ready", "ready"] and
      same_run["how"] == "unchanged since it was prepared" and same_run["already"] == 2 and
      rows() == old_keys and manifest.read_bytes().count(b"decoder 10.0.1.0\n") == 2,
      "same decoder reuses AAC and preserves both version rows while ALAC has none")

new_quick = results("--quick", aac, later, alac, **new_mf)
new_estimate = results("--estimate", aac, later, alac, **new_mf)
new_run = rerun(**new_mf)
check(new_quick == new_estimate == ["prepare", "prepare", "ready"] and
      new_run["written"] == 2 and new_run["already"] == 0 and len(rows() - old_keys) == 2 and
      results("--quick", aac, later, alac, **new_mf) == ["ready", "prepare", "ready"],
      "changed decoder rechecks AAC only and a partial run updates only the AAC it decoded")

# Remove the optional rows to simulate an earlier Windows helper.
manifest.write_bytes(b"".join(line for line in old_manifest.splitlines(keepends=True)
                              if not line.startswith(b"decoder ")))
refusal = dict(new_mf, RBSP_TEST_PROBE_REFUSE="probe refused")
legacy_quick = results("--quick", aac, **refusal)
legacy_estimate = results("--estimate", aac, **new_mf)
legacy_refused = results("--estimate", aac, **refusal)
legacy_run = next(row for row in invoke(aac, **new_mf) if "file" in row)
check(b"decoder 10.0.1.0\n" in old_manifest and
      legacy_quick == legacy_estimate == ["ready"] and legacy_refused == ["skipped"] and
      legacy_run["how"] == "unchanged since it was prepared",
      "legacy AAC uses the full estimate's probe and survives a missing version")

# Main stored a decoder version before it had source-rate or AAC-length evidence.
aac_suffix = b" " + os.fsencode(aac) + b"\n"
manifest.write_bytes(b"".join(
    line.replace(b" 2 2 hz=44100 ", b" 1 2 ", 1)
    if line.startswith(b"file ") and line.endswith(aac_suffix) else line
    for line in old_manifest.splitlines(keepends=True)))
main_quick = results("--quick", aac, **old_mf)
main_estimate = invoke("--estimate", aac, **old_mf)[0]
upgraded = rerun(**old_mf)
check(main_quick == ["unchecked"] and main_estimate["result"] == "prepare" and
      main_estimate["approximate"] and upgraded["result"] == "prepared" and
      upgraded["how"] != "unchanged since it was prepared" and
      results("--quick", aac, **old_mf) == ["ready"] and
      results("--estimate", aac, **old_mf) == ["ready"] and
      manifest.read_bytes().count(b"decoder 10.0.1.0\n") == 2,
      "an AAC record from main rechecks once and retains its decoder version on upgrade")

print(f"kept-cli-host: {passed} passed, 0 failed")
