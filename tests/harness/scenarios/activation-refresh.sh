# scenario: coming back to the app shows what changed meanwhile (the lights, the buttons)
# commit: 6402636
# tag: first
# compare: exact
# verbs: both
# While the app is in the background, Demucs v4's model appears in rekordbox's folder (as an
# install from elsewhere would leave it): on activation the light turns green, Install Demucs v4
# turns off and Uninstall Demucs v4 on, at once, with no relaunch.
setup() { fact_welcomed; }
steps() {
  launch
  settle
  wait_for 3000 "c.plusIcon.img=off"
  finder_front
  fact_plus_on
  mark "fact changed: Demucs v4 in rekordbox"
  activate
  wait_for 3000 "c.plusIcon.img=on"
  wait_for 3000 "c.installPlusBtn.en=0"
  settle
}
