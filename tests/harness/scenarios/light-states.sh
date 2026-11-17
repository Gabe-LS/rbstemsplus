# scenario: the two lights, their tints and tooltips, the buttons and the status line through the
# states a user meets, each reached while the app is in the background and shown on coming back
# commit: a386d7f, 5136844, 23403a9, 6402636, e26f9a1, 0b48e80
# tag: first
# compare: exact
setup() { fact_welcomed; }
steps() {
  launch
  settle
  step() { finder_front; "$@"; mark "fact: $*"; VERB=reopen activate; settle; }
  step fact_plus_on                                           # Demucs v4 on
  step fact_cache_on                                          # Stems Cache in rekordbox, not chosen here: yellow
  step fact_chosen 1 1                                        # chosen: green with Demucs v4
  step fact_engine ""                                         # no STEMS Engine (matters only with rekordbox's own model)
  step eval 'cp -p "$H/pristine/demucs3_model/hdemucs.onnx" "$MODELDIR/hdemucs.onnx"; own "$MODELDIR"'   # rekordbox's own model back
  step fact_engine 0002                                       # the engine back: the model's checksum decides
  step fact_config "rekordbox_model=0"                        # rekordbox's own model turned off in config.ini: yellow
  step fact_config "rekordbox_model=1"
  step fact_rekordbox_version 7.9.0.0001                      # an unsupported rekordbox: install buttons off, the reason
  tab Status; settle
}
