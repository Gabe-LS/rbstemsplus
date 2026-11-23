"""Compare the release checker with C on synthetic fixtures, then exercise build.sh's gate."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[3]


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expect_failure(result, message):
    assert result.returncode == 1 and message in result.stderr, (result.returncode, result.stdout, result.stderr)


def main():
    test, scratch = map(Path, sys.argv[1:])
    run = subprocess.run([test, scratch], check=True, text=True, stdout=subprocess.PIPE)
    *pairs, c_result = run.stdout.splitlines()
    print(c_result, flush=True)
    digests = dict(line.split() for line in pairs)

    originals = scratch / "originals with spaces"
    for version in ("7.2.19", "7.2.20"):
        (originals / version).mkdir(parents=True)
    left, right = (originals / version / "onnxruntime.dll" for version in ("7.2.19", "7.2.20"))
    helper = ROOT / "scripts/win-ort-code.py"

    # Run the real preflight with a synthetic list. The missing model reference stops even a
    # successful preflight before packaging starts.
    build_root = scratch / "build-root"
    build = build_root / "scripts/build.sh"
    build.parent.mkdir(parents=True)
    shutil.copyfile(helper, build.parent / helper.name)
    (build_root / "VERSION").write_text("1.5.0\n")
    (build_root / "app/Sources").mkdir(parents=True)
    (build_root / "app/Sources/Paths.swift").write_text("// Intentionally no model reference.\n")
    subprocess.run(["git", "init", "-q", build_root], check=True)
    subprocess.run(["git", "-C", build_root, "-c", "user.name=Test", "-c", "user.email=test@example.org",
                    "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null",
                    "commit", "-q", "--allow-empty", "-m", "Test preflight"], check=True)
    source = (ROOT / "scripts/build.sh").read_text()
    shutil.copyfile(scratch / "pe32-plus.dll", left)
    env = {"PATH": os.environ["PATH"], "TMPDIR": str(scratch.resolve())}
    code = digests["pe32-plus"]
    unmeasured = "UNMEASURED_PE_CODE_SHA256"

    def build_with(*flags, verify_originals=False):
        build_env = dict(env)
        if verify_originals:
            build_env["RBSTEMSPLUS_WIN_ORT_DIR"] = str(originals)
        return subprocess.run(["bash", build, "--test", "http://localhost", *flags], env=build_env,
                              text=True, capture_output=True)

    def set_bindings(right_sha, left_code=code, right_code=code):
        bindings = [f"7.2.19={sha256(left)}:{left_code}", f"7.2.20={right_sha}:{right_code}"]
        table = 'compat_win_ort_bind="' + " ".join(bindings) + '"'
        script = re.sub(r'^compat_win_ort_bind="[^"]*"$', table, source, flags=re.MULTILINE)
        build.write_text(script)
        return bindings

    windows = ("--windows", "--win-test", "https://localhost")
    model_check = "the model SHA-256 here isn't in app/Sources"
    shutil.copyfile(scratch / "resigned.dll", right)
    set_bindings(sha256(right))
    expect_failure(build_with(*windows), model_check)
    expect_failure(build_with(*windows, verify_originals=True), model_check)

    set_bindings(sha256(right), right_code="f" * 64)
    expect_failure(build_with(*windows), "different PE code digests")
    set_bindings(sha256(right), right_code="invalid")
    expect_failure(build_with(*windows), "invalid PE code digest")
    set_bindings(sha256(right), "f" * 64, "f" * 64)
    expect_failure(build_with(*windows, verify_originals=True), "does not match the recorded PE code digest")

    bindings = set_bindings(sha256(right), unmeasured, unmeasured)
    result = build_with(*windows)
    expect_failure(result, model_check)
    assert "warning:" in result.stderr and unmeasured in result.stderr, result.stderr
    helper_args = [sys.executable, "-I", helper, "--recorded"]
    expect_failure(subprocess.run([*helper_args, *bindings], text=True, capture_output=True), unmeasured)
    result = subprocess.run([*helper_args, "--test", *bindings], check=True, text=True, capture_output=True)
    assert result.stdout.strip() == "0" * 64, result.stdout
    expect_failure(build_with(*windows, verify_originals=True), unmeasured)

    shutil.copyfile(scratch / "code-change.dll", right)
    set_bindings(sha256(right))
    expect_failure(build_with(*windows, verify_originals=True), "different PE code digests")

    shutil.copyfile(scratch / "resigned.dll", right)
    expect_failure(build_with(*windows, verify_originals=True), "SHA-256 does not match the binding")
    set_bindings(sha256(right))
    right.unlink()
    left.unlink()
    expect_failure(build_with(*windows), model_check)
    expect_failure(build_with(*windows, verify_originals=True), "missing original")
    expect_failure(build_with(), model_check)

    for name, digest in digests.items():
        shutil.copyfile(scratch / f"{name}.dll", left)
        result = subprocess.run([sys.executable, "-I", helper, "--originals", originals, f"7.2.19={sha256(left)}"],
                                check=True, text=True, capture_output=True)
        assert result.stdout.strip() == digest, (name, result.stdout, digest)
    print(f"ORT release check: {len(digests)} matching C digests, 14 preflight checks passed")


if __name__ == "__main__":
    main()
