# scenario: the Stems Cache update offer, when the bridge in rekordbox isn't the latest
# payload.json's: at launch from the saved payload.json (asked once, Not Now changes nothing, not
# asked again on coming back); then, with none saved, asked once the payload.json downloaded at
# launch arrives
# commit: 272125a, 10e616d
# tag: first
# compare: exact
# server: make variant --bridge-variant
setup() { fact_welcomed; fact_cache_on; fact_chosen 0 1; fact_config "rekordbox_model=1"; fact_engine 0002; fact_saved_payload; }
steps() {
  launch
  next_dialog 15000; settle              # Stems Cache update (from the saved payload.json)
  answer "Not Now"; wait_no_dialog 5000; settle
  finder_front
  VERB=reopen activate
  settle
  check '[ "$(count_of dialog)" = 1 ]' "asked once this run"
  quit_app
  rm -rf "$SUP/payload"
  launch
  next_dialog 30000; settle              # asked once the download ended
  answer "Not Now"; wait_no_dialog 5000; settle
  check '[ -f "$SUP/payload/payload.json" ]' "the payload.json downloaded at launch was saved"
}
