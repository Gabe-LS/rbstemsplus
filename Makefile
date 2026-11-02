# Shortcuts for the build scripts (run from the repository folder). The scripts do the work.
#   make          every release asset into dist/ (scripts/build.sh; scripts/release.sh builds,
#                 signs and uploads a release itself)
#   make app      only dist/rbstemsplus-app.zip
#   make bridge   only the bridge, into build/bridge/
#   make check    quick checks: shell syntax, the two Info.plist files
#   make test     the app's safety checks (app/Tests), built from the app's sources and run
#   make bridge-test  the bridge's tests (bridge/tests/run.sh, disk images included), again with
#                 ASan and UBSan, and the fuzz targets; fails when any test is skipped (a release
#                 needs it: docs/RELEASING.md). Run outside a sandbox: it attaches disk images.
#   make clean    remove build/, dist/ and dist-test/ (they may hold a copy of the model)

.PHONY: all app bridge check test bridge-test clean

all:
	scripts/build.sh

app:
	scripts/build-app.sh

bridge:
	bridge/build.sh build/bridge

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
	plutil -lint app/Info.plist
	plutil -lint app/Watcher-Info.plist

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

clean:
	rm -rf build dist dist-test
