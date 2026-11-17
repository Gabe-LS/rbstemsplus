# scenario: folders the user closed stay closed across list reloads (Check Again), and a playlist
# chosen from the Status tab opens the folder it is in
# commit: a310fc2, d8d5e64, b606a39
# tag: Prepare
# compare: properties
# allow: status.table.r0 status.table.sel
# (the Status tab's row gets its name (--playlists) and its count (--tracks) from two checks that
# finish in either order in A: compared by final values and as a subsequence (a frame more or
# fewer in either run); the table's selection, reloaded under the second check, is shown again
# or not by the same race (A's two recordings differ): those two only, by final value and as a
# subsequence; status-sets is that tab's scenario)
setup() { fixture_prepare_ready; fixture_library; estimate_friday; fact_kept 2 9 "$(track friday-a)"; }
steps() {
  launch
  settle
  tab Prepare
  wait_for 5000 "prepare.outline.rows=5"
  settle
  cmd collapse 1
  wait_for 3000 "prepare.outline.rows=3"
  select_playlist 3
  settle
  click prepare.checkBtn                # the list read again: Sets stays closed
  settle
  tab Status
  wait_for 5000 "status.table.rows=1"
  settle
  cmd select-set 0; settle              # the click before the double-click (its own frames)
  cmd doubleclick-set 0                 # Prepare Again on Friday, inside the closed folder
  wait_for 5000 "tab.sel=1"
  wait_for 5000 "prepare.outline.rows=5"
  settle
}
