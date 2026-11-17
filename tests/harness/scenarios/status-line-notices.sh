# scenario: the status line's notices between actions: the saved stems folder made a link while
# the app runs, shown on coming back
# commit: a1e7428, f89a2a0 (line order), 22e8b33
# tag: first
# compare: exact
setup() { fact_welcomed; }
steps() {
  launch
  settle
  finder_front
  fact_cache_link
  VERB=reopen activate
  settle
}
