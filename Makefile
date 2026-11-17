# Shortcuts for the build scripts (run from the repository folder). The scripts do the work.
#   make          every release asset into dist/ (scripts/build.sh; scripts/release.sh builds,
#                 signs and uploads a release itself)
#   make app      only dist/rbstemsplus-app.zip
#   make windows  only the Windows files (scripts/build-windows.sh --check-repro: built twice,
#                 every file the same bytes) into dist/: rbstemsplus-windows.zip,
#                 rbstemsplus-bridge-win-x64.dll and windows.env; their SHA-256s in build/win.sha256
#   make bridge   only the bridge, into build/bridge/
#   make helper   the prepare helper (helper/build.sh), into build/helper/: not released yet
#   make check    quick checks: shell syntax, the two Info.plist files, the bundle gates' tests,
#                 the G3 tart run's selftest (tests/mac/g3-test.sh: no VM)
#   make test     the app's safety checks (app/Tests), built from the app's sources and run
#   make bridge-test  the bridge's tests (bridge/tests/run.sh, disk images included), again with
#                 ASan and UBSan, and the fuzz targets; fails when any test is skipped (a release
#                 needs it: docs/RELEASING.md). Run outside a sandbox: it attaches disk images.
#   make release-test  scripts/release.sh end to end in a sandbox (tests/release/sandbox.sh:
#                 fake repositories, a fake gh, a throwaway key, fake builds): branch-test.sh (main,
#                 --branch, the pin), carry-test.sh (the Mac carry-forward, --plan, --mac),
#                 windows-assets-test.sh (build.sh's Windows files in a plan's shape),
#                 carry-file-guard-test.sh (build-windows.sh's --carry-file guard),
#                 windows-test.sh (the Windows assets, carry-forward and VM record, T13); and
#                 tests/mac/bundle-compare-test.sh (the compare on small real bundles); and
#                 build-lists-test. Run outside a sandbox: it needs a pseudo-terminal and actool.
#   make build-lists-test  helper/build.sh's and bridge/build.sh's input lists when the depfile
#                 parser fails or gives none (tests/mac/build-lists-test.sh: real builds, a minute)
#   make carry-test   the carry-forward with the real builds (tests/release/mac-build-test.sh: the
#                 input lists, the FLAC_VERSION-only and header-only commits, the stamped app,
#                 build.sh --test --plan). Several minutes; outside a sandbox; needs the model.
#   make mac-unchanged  the merge check (tests/mac/unchanged.sh): the Mac app, helper and bridge
#                 built from the merge base with main and from HEAD are the same (BASE=COMMIT for
#                 another base). Several minutes; outside a sandbox.
#   make core-test    the shared C core's tests (core/build.sh; linked into every app build): the
#                 unit tests as shipped (mac) and with CORE_WIN_POLICY (win): plain and ASan+UBSan
#                 with the release checks, ASan+UBSan and TSan with CORE_DEBUG; the per-OS rows
#                 check; the Mac input lists (core/tests/mac-inputs.sh: no Windows-only or test
#                 file on them); core.h imported by Swift; the G1 API spike's host smoke test
#                 (tests/mac/spike-smoke.sh); the AppKit shell's host tests (tests/mac/shell-test.sh:
#                 the dialog presenter, the host table, render, the menus); the fuzz runs (mac and
#                 win). The unit tests compare the core's pure functions with the Swift app's
#                 (core/tests/gen-vectors.sh builds the vectors from its frozen copy,
#                 core/tests/swift/oracle: app/Sources at 56d7c11 plus freeze-time fixes, into
#                 build/core); every function still in app/Sources must be its oracle copy byte
#                 for byte (core/tests/oracle-same.py), and Create Report's tool calls 1.3.0's
#                 (core/tests/report-tools.py). First, the masking's Unicode tables
#                 (core/mask_tables.h) must be what this Mac's ICU gives (core/build.sh tables).
#                 Run outside a sandbox (swiftc writes its module cache in the user's folder).
#   make common-test  common/'s unit tests (common/tests/run.sh): plat_mac.c plain, with ASan and
#                 UBSan and with TSan; the shared modules against tests/config-fixtures.txt and
#                 tests/marker-fixtures.txt; the core's unit tests built on common/plat.h instead
#                 of its shim
#   make helper-win  the Windows prepare helper's parts (helper/build-win.sh), into build/helper-win/:
#                 x64 with the pinned llvm-mingw, -Werror, and its gates (imports, manifest, key,
#                 sin/pow, reproducible); the exes are for the VM
#   make helper-win-test  its Mac tests: the platform calls (win-plat.sh), SQLCipher with LibTomCrypt
#                 (win-sqlcipher.sh), the decoders against AudioToolbox (win-decode.sh, win-alac.sh,
#                 win-mp3.sh). Run outside a sandbox (the account's temporary folder, afconvert).
#   make winc-test   the Windows app's C modules (win/c/tests/run.sh, app.md §5.1-5.4, §7): their
#                 Mac-native tests against today's Swift's fixtures, plain and with ASan and UBSan,
#                 the fuzz driver, and the Windows x64 and arm64 builds with their import gate;
#                 then the core DLL (core/build.sh dll, release and --test-hooks) with its gates.
#                 Run outside a sandbox (swiftc writes its module cache in the user's folder).
#   make winboot-test  scripts/bootstrap.ps1's tests in pwsh (tests/win/bootstrap/run.sh, app.md
#                 W12): its rules, texts, signature, payload, records and the install on a temp folder
#                 with a fake Windows. Needs pwsh (brew install powershell); 10 minutes at most.
#   make winvm-test  tests/win/vm-run.sh's offline tests (tests/win/vm-run-test.sh, release.md T16):
#                 the revert, the steps, the timeouts, the record against a fake prlctl; no VM
#   make clean    remove build/, dist/ and dist-test/ (they may hold a copy of the model)

.PHONY: all app windows bridge helper helper-win helper-win-test check test bridge-test release-test build-lists-test carry-test mac-unchanged core-test common-test winc-test winboot-test winvm-test win-shell clean

all:
	scripts/build.sh

app:
	scripts/build-app.sh

windows:
	scripts/build-windows.sh --check-repro dist

bridge:
	bridge/build.sh build/bridge

helper:
	helper/build.sh build/helper

helper-win:
	helper/build-win.sh build/helper-win

helper-win-test:
	helper/tests/win-plat.sh
	helper/tests/win-sqlcipher.sh
	helper/tests/win-decode.sh
	helper/tests/win-alac.sh
	helper/tests/win-mp3.sh

check:
	bash -n scripts/build.sh
	bash -n scripts/build-app.sh
	bash -n scripts/bootstrap.sh
	bash -n scripts/release.sh
	bash -n scripts/make-signing-keys.sh
	bash -n tests/root/vm-root-tests.sh
	sh -n bridge/build.sh
	bash -n bridge/tests/run.sh
	bash -n bridge/tests/location.sh
	sh -n helper/build.sh
	sh -n helper/build-win.sh
	sh -n scripts/deps.sh
	scripts/deps.sh check
	sh -n helper/tests/win-plat.sh
	sh -n helper/tests/win-sqlcipher.sh
	bash -n helper/tests/win-decode.sh
	bash -n helper/tests/win-decode-vm.sh
	sh -n helper/tests/win-alac.sh
	bash -n helper/tests/win-mp3.sh
	python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' helper/tests/win-decode-fixtures.py
	bash -n bridge/tests/perf.sh
	bash -n helper/tests/perf.sh
	bash -n tests/perf/b0.sh
	python3 -I tests/perf/perfstats.py selftest > /dev/null
	python3 -I tests/perf/rules.py check
	helper/tests/same-as-bridge.sh
	bash -n tests/mac/minos.sh
	bash -n tests/mac/repro.sh
	bash -n tests/release/branch-test.sh
	bash -n tests/release/sandbox.sh
	bash -n tests/release/carry-test.sh
	bash -n tests/release/mac-build-test.sh
	bash -n tests/mac/unchanged.sh
	bash -n tests/mac/bundle-compare-test.sh
	bash -n scripts/bundle-compare.sh
	python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' scripts/mac-inputs.py
	tests/release/mac-inputs-test.sh
	bash -n tests/mac/build-lists-test.sh
	bash -n tests/mac/bundle-files.sh
	bash -n tests/mac/swap-selftest.sh
	bash -n tests/mac/spike-smoke.sh
	bash -n tests/mac/shell-test.sh
	bash -n tests/mac/static-gates.sh
	tests/mac/gates-test.sh
	bash -n tests/mac/g3.sh
	bash -n tests/mac/g3-guest.sh
	tests/mac/g3-test.sh
	sh -n core/build.sh
	sh -n core/tests/dll-keys.sh
	sh -n core/tests/os-rows.sh
	bash -n core/tests/mac-inputs.sh
	sh -n common/tests/run.sh
	sh -n tests/win/blocks/build.sh
	sh -n core/tests/gen-vectors.sh
	sh -n win/c/tests/run.sh
	sh -n tests/win/bootstrap/run.sh
	sh -n win/c/tests/gen-fixtures.sh
	python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' win/c/tests/gen-zips.py
	sh -n win/c/tests/watch.sh
	sh -n win/c/tests/app.sh
	sh -n win/admin/tests/run.sh
	sh -n win/admin/build.sh
	sh -n win/shell/build.sh
	bash -n scripts/build-windows.sh
	bash -n scripts/windows-assets.sh
	bash -n tests/release/windows-assets-test.sh
	python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' scripts/win-carry.py
	python3 -I -c 'import ast, sys; ast.parse(open(sys.argv[1]).read())' scripts/win-version.py
	bash -n win/shell/vm/run.sh
	python3 -I core/tests/texts-in-swift.py
	python3 -I core/tools/win_texts_table.py > /dev/null
	plutil -lint app/Info.plist
	plutil -lint app/Watcher-Info.plist
	tests/harness/selftest.sh
	@# the release notes, unwrapped for GitHub (release.sh), keep every word
	@for f in docs/release-notes/*.md; do \
	  [ "$$(tr -s ' \n' '  ' < "$$f")" = "$$(python3 -I scripts/unwrap-notes.py "$$f" | tr -s ' \n' '  ')" ] \
	    || { echo "unwrap-notes.py changes the words of $$f"; exit 1; }; \
	done; echo "release notes: unwrap keeps every word"

test:
	mkdir -p build/test
	# no compiled-in signing keys: the tests make and pass their own (scripts/build-app.sh writes the real list)
	printf '%s\n' 'let compiledKeys: [(name: String, der: String)] = []' > build/test/Keys.swift
	# the version, as scripts/build-app.sh compiles it in (the oldest payload.json the app accepts)
	printf 'let compiledVersion = "%s"\n' "$$(tr -d '[:space:]' < VERSION)" > build/test/Version.swift
	xcrun swiftc -sdk "$$(xcrun --sdk macosx --show-sdk-path)" \
	  $(filter-out app/Sources/main.swift,$(wildcard app/Sources/*.swift)) build/test/Keys.swift build/test/Version.swift $(wildcard app/Tests/*.swift) -o build/test/rbsp-test
	build/test/rbsp-test
	# again under the hardened runtime, as the app runs: processes, sudo's tools, CryptoKit (the
	# root scripts' runs on fake rekordbox installs, bash children of the tests, only once)
	cp build/test/rbsp-test build/test/rbsp-test-runtime
	codesign -f -s - --options runtime build/test/rbsp-test-runtime
	build/test/rbsp-test-runtime --skip-root-runs

bridge-test:
	mkdir -p build
	# each step's own exit status stops make; its output is kept for the summary
	REQUIRE=1 bridge/tests/run.sh > build/bridge-test.run.log 2>&1; rc=$$?; cat build/bridge-test.run.log; exit $$rc
	REQUIRE=1 SANITIZE=1 bridge/tests/run.sh > build/bridge-test.san.log 2>&1; rc=$$?; cat build/bridge-test.san.log; exit $$rc
	bridge/build.sh --fuzz build/fuzz
	build/fuzz/fuzz 200000 > build/bridge-test.fuzz.log 2>&1; rc=$$?; cat build/bridge-test.fuzz.log; exit $$rc
	# the line to record with the release commit: both runs' counts, the fuzz's seed and result
	@echo "bridge-test: run.sh $$(grep -E '^[0-9]+ passed' build/bridge-test.run.log); SANITIZE=1 $$(grep -E '^[0-9]+ passed' build/bridge-test.san.log); $$(grep -E '^fuzz: no failures' build/bridge-test.fuzz.log) ($$(grep -oE 'seed [0-9]+' build/bridge-test.fuzz.log))"

release-test: build-lists-test
	tests/release/windows-assets-test.sh
	tests/release/carry-file-guard-test.sh
	tests/release/branch-test.sh
	tests/release/carry-test.sh
	tests/release/windows-test.sh
	tests/mac/bundle-compare-test.sh

build-lists-test:
	tests/mac/build-lists-test.sh

carry-test:
	tests/release/mac-build-test.sh

mac-unchanged:
	tests/mac/unchanged.sh $(BASE)

core-test:
	core/build.sh tables build/core > /dev/null
	@cmp -s build/core/mask_tables.h core/mask_tables.h \
	  || { echo "core/mask_tables.h isn't what this Mac's ICU gives: review build/core/mask_tables.h and commit it as core/mask_tables.h"; exit 1; }
	python3 -I core/tests/oracle-same.py
	python3 -I core/tests/report-tools.py
	core/tests/gen-vectors.sh build/core > /dev/null
	@for v in "plain mac" "plain win" "asan mac" "asan win" "asanrel mac" "asanrel win" "tsan mac" "tsan win" "tsanrel mac" "tsanrel win"; do \
	  set -- $$v; core/build.sh test build/core $$1 $$2 > /dev/null && build/core/core-test-$$1-$$2 || exit 1; \
	done
	core/tests/os-rows.sh
	core/tests/mac-inputs.sh
	core/build.sh swift build/core > /dev/null && build/core/core-smoke
	tests/mac/spike-smoke.sh
	tests/mac/shell-test.sh
	core/build.sh fuzz build/core mac > /dev/null && build/core/core-fuzz-mac 20000
	core/build.sh fuzz build/core win > /dev/null && build/core/core-fuzz-win 20000

common-test:
	common/tests/run.sh build/common

winc-test:
	win/c/tests/run.sh build/winc
	core/tests/dll-keys.sh
	core/build.sh dll build/core-dll
	core/build.sh dll build/core-dll --test-hooks

winboot-test:
	tests/win/bootstrap/run.sh

winvm-test:
	tests/win/vm-run-test.sh

win-shell:
	win/shell/build.sh build/shell

clean:
	rm -rf build dist dist-test
