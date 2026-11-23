#!/bin/sh
# Check the watcher's linker map for the sections that hold its test-only code and data.
set -eu
[ "$#" = 2 ] || exit 2
case "$1" in release|test) ;; *) exit 2 ;; esac
awk -v mode="$1" '
  /(^|[[:space:]])wWinMain([[:space:]]|$)/ { watcher = 1 }
  /\$rbsp_test_watch_/ { test_sections = 1 }
  /\$rbsp_test_watch_paths\)/ { paths = 1 }
  /\$rbsp_test_watch_notify\)/ { notify = 1 }
  /\$rbsp_test_watch_programs\)/ { programs = 1 }
  /\$rbsp_test_watch_text\)/ { text = 1 }
  END {
    if (!watcher) { print "FAIL watcher map missing wWinMain: " FILENAME; exit 1 }
    if (mode == "release" && test_sections) {
      print "FAIL release watcher contains test-only sections: " FILENAME; exit 1
    }
    if (mode == "test" && !(paths && notify && programs && text)) {
      print "FAIL test watcher map missing test-only sections: " FILENAME; exit 1
    }
    print "ok   watcher map (" mode "): " FILENAME
  }
' "$2"
