# scenario: no check during an action, and which actions check again at their end: a short wait
# (the complete uninstall's "Checking where stems are saved…", then Cancel) leaves the Prepare tab's
# estimate as it is; Install Demucs v4 from the local server, with the Prepare tab brought back to
# the front while it runs (today the tab checks as it appears: recorded) and its end under the
# Prepare tab checks it again
# commit: 1ab82da, 61b7375, 78ae5d9
# tag: Prepare
# compare: properties
# server: dist-test
# allow: c.status.str
# mask: Downloading the Demucs v4 model… \d+%
# (the download's progress lines depend on the transfer: by their final value and as a
# subsequence of A's; every other field exactly)
setup() { fixture_prepare_ready; fixture_library; estimate_friday; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  select_playlist 2
  wait_for 5000 "prepare.prepareBtn.en=1"
  settle
  local n; n="$(calls_of estimate-2)"
  # a short wait (inBackground), then the confirmation cancelled
  menu "menu.App.Uninstall RB Stems Plus…"
  next_dialog 10000; settle
  answer Cancel; settle
  check '[ "$(calls_of estimate-2)" = "$n" ]' "a short wait doesn't check again"
  # an install: the Manage tab comes to the front; the Prepare tab back in front while it runs
  menu "menu.Manage.Demucs v4.Install"
  next_dialog 5000; settle
  answer Install
  wait_for 10000 "tab.sel=2"
  tab Prepare
  wait_for 5000 "tab.sel=1"; settle 300
  # today the tab shown during the action checks as it appears (recorded, not asserted)
  local during; during="$(calls_of estimate-2)"
  echo "note: estimates after bringing the Prepare tab back during the install: $((during - n))" >> "$RUN/result"
  wait_for 300000 "c.spinner.anim=0"
  helper_calls estimate-2 $((during + 1))
  wait_for 10000 "prepare.prepareBtn.en=1"
  settle
}
