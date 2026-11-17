// The Status tab's view model (app.md §4.2, W8b; the Mac shell's ShellStatusTab, app/Shell/
// ShellTabs.swift, which StatusTab.updateButtons() drew): the prepared playlists (T_STATUS_SETS),
// the selection, Prepare Again, Stop Keeping… and the run's line beside them, every value from the
// S_STATUS section, every user action an event to the core. No WPF here (the Mac tests drive it
// with a fake snapshot).
//  - Scalars raise PropertyChanged only when their value changed.
//  - Rows are deltas (core.h Tables): a new list_gen (or fewer rows) rebuilds the list once;
//    otherwise only the changed rows are updated in place and appended rows added.
//  - Echo guard: while Apply runs, nothing reaches the core (the Mac's applying). User intent
//    comes only from the input side: SelectedIndex set by the list's TwoWay binding (a click, a
//    key), sent only when it differs from the selection shown; a row's double click and the
//    buttons are clicks.
// Streaming tracks are never counted, listed or reported here: the rows are the core's.
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using RbStemsPlus.Core;

namespace RbStemsPlus.Shell
{
    public sealed class SetRow : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        private string name = "", tracks = "", until = "", state = "";
        public string Name { get => name; internal set { if (name != value) { name = value; Raise(nameof(Name)); } } }
        public string Tracks { get => tracks; internal set { if (tracks != value) { tracks = value; Raise(nameof(Tracks)); } } }
        public string Until { get => until; internal set { if (until != value) { until = value; Raise(nameof(Until)); } } }
        public string State { get => state; internal set { if (state != value) { state = value; Raise(nameof(State)); } } }
        /// What Narrator reads for the row (app.md §4.7): its cells as shown, the empty ones left out
        public string Spoken => Cells.Join(name, tracks, until, state);
        public override string ToString() => Spoken;
        private void Raise(string n)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Spoken)));
        }
    }

    public sealed class StatusModel : INotifyPropertyChanged
    {
        /// the Status tab's click ids (status_tab.c's status_event)
        public const string PrepareAgainId = "prepare_again", StopKeepingId = "stop_keeping";

        public event PropertyChangedEventHandler PropertyChanged;
        private readonly Action<string> send;
        private readonly Dictionary<string, object> values = new Dictionary<string, object>();
        private ulong setsGen = ulong.MaxValue;
        private bool applying;
        private int selected = -1;

        /// send: one event to the core ({"ev":...} JSON), from the UI thread, outside a render.
        public StatusModel(Action<string> send) { this.send = send; }

        public ObservableCollection<SetRow> Sets { get; } = new ObservableCollection<SetRow>();

        /// the probe's (TEST builds): once per render that changed the list, as the Mac shell's
        /// reloadData (whole: the row count) or reloadData(forRowIndexes:) (the first changed row)
        public Action<bool, int> SetsRendered;

        private T Get<T>(string name, T fallback = default(T)) => values.TryGetValue(name, out var v) ? (T)v : fallback;
        private void Set<T>(string name, T value)
        {
            if (values.TryGetValue(name, out var old) && Equals(old, value)) return;
            values[name] = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        /// Prepare Again and Stop Keeping…: off until the first render gives their fields
        public bool AgainEnabled => Get<bool>(nameof(AgainEnabled));
        public bool StopEnabled => Get<bool>(nameof(StopEnabled));
        /// the line beside the buttons: the run's summary, "" none
        public string RunLine => Get(nameof(RunLine), "");

        /// The list's selection (-1 none). The render sets it inside the echo guard; the list's
        /// TwoWay binding (the user's click or key) sends {"ev":"select"} when it differs from the
        /// selection shown (the Mac's selectedRow != sel). A rebuild's own reset (inside the
        /// guard) sends nothing.
        public int SelectedIndex
        {
            get => selected;
            set
            {
                if (selected == value) return;
                selected = value;
                if (applying) return;
                send("{\"ev\":\"select\",\"table\":\"T_STATUS_SETS\",\"row\":" + Json.Num(value) + ",\"gen\":" + Json.Num(setsGen) + "}");
            }
        }

        /// One render's S_STATUS pass (main, inside render: accessors only, no event sent).
        internal void Apply(IPrepareSource s)
        {
            applying = true;
            try
            {
                ApplySets(s);
                int sel = (int)s.I64(Field.F_STATUS_SELECTED_ROW);
                if (sel < 0 || sel >= Sets.Count) sel = -1;
                if (selected != sel)
                {
                    selected = sel;
                    PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(SelectedIndex)));
                }
                Set(nameof(AgainEnabled), s.Flag(Field.F_STATUS_AGAIN_ENABLED));
                Set(nameof(StopEnabled), s.Flag(Field.F_STATUS_STOP_ENABLED));
                Set(nameof(RunLine), s.Str(Field.F_STATUS_RUN_LINE));
            }
            finally { applying = false; }
        }

        private void ApplySets(IPrepareSource s)
        {
            int n = s.Rows(Table.T_STATUS_SETS);
            ulong gen = s.ListGen(Table.T_STATUS_SETS);
            if (s.Reload(Table.T_STATUS_SETS) || gen != setsGen || n < Sets.Count)
            {
                setsGen = gen;
                Sets.Clear();
                for (int r = 0; r < n; r++) Sets.Add(Row(s, r, new SetRow()));
                SetsRendered?.Invoke(true, n);
                return;
            }
            int[] changed = s.Changed(Table.T_STATUS_SETS);
            foreach (int r in changed) if (r >= 0 && r < Sets.Count) Row(s, r, Sets[r]);
            if (changed.Length > 0) SetsRendered?.Invoke(false, changed[0]);
            for (int r = Sets.Count; r < n; r++) Sets.Add(Row(s, r, new SetRow()));
        }

        private static SetRow Row(IPrepareSource s, int r, SetRow row)
        {
            row.Name = s.CellStr(Column.C_SET_NAME, r);
            row.Tracks = s.CellStr(Column.C_SET_TRACKS, r);
            row.Until = s.CellStr(Column.C_SET_UNTIL, r);
            row.State = s.CellStr(Column.C_SET_STATE, r);
            return row;
        }

        // ------------------------------------------------------------------ user intent

        /// The buttons: Prepare Again, Stop Keeping… (the core checks Stop Keeping…'s field; Prepare Again, as the Mac's double click, is not gated).
        public void Click(string id)
        {
            if (applying) return;
            send(Json.Click(id));
        }

        /// A row's double click (the Mac's doubleAction, only on a row): Prepare Again's click.
        public void RowDoubleClicked()
        {
            if (applying) return;
            send(Json.Click(PrepareAgainId));
        }
    }
}
