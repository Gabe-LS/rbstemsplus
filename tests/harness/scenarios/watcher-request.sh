# scenario: the watcher's Reinstall Now request: an hour-old one is dropped silently; a fresh one,
# with rekordbox open, asks to quit it (Try Again / Cancel) instead of refusing; coming back to
# the app while that dialog is up doesn't ask again
# commit: 4ac419e, 8d74bc0, a2f43d7, ae2f286
# tag: first
# compare: exact
# verbs: both
setup() { fact_welcomed; fact_chosen 1 0; fact_request 4000; }
steps() {
  launch
  settle
  check '[ ! -e "$SUP/request" ]' "the hour-old request was dropped"
  check '[ "$(count_of dialog)" = 0 ]' "no dialog for an old request"
  finder_front
  fact_request
  fact_rekordbox_open
  activate                               # the request is checked on coming back
  next_dialog 10000; settle
  finder_front
  activate                               # the dialog is up: not asked again
  settle
  answer Cancel; wait_no_dialog 5000; settle
  fact_rekordbox_closed
}
