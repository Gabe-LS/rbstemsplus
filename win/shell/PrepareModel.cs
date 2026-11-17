// The Prepare tab's view model (app.md §4.2, W8b; the Mac shell's ShellPrepareTab, app/Shell/
// ShellTabs.swift, which PrepareTab.update() drew): every value from the S_PREPARE section, every
// user action an event to the core. No WPF here (the Mac tests drive it with a fake snapshot).
//  - Scalars raise PropertyChanged only when their value changed.
//  - Rows are deltas (core.h Tables): a new list_gen (or a different row count for the outline)
//    rebuilds a list once; otherwise only the changed rows are updated in place and appended rows
//    added, so expansion, selection and scroll stay where they are ("rows never move").
//  - The outline is a tree of OutlineRow (pre-order rows, a parent before its children).
//  - Echo guard: while Apply runs, nothing reaches the core (the Mac's reloadingList). User intent
//    comes only from the input side: OutlineRow.IsSelected/IsExpanded set by a TwoWay binding's
//    target-to-source update (a click, a key, the expander), never from the render, and a
//    selection reaches the core only when it differs from the one shown (the Mac shell's
//    shownSelection: a collapse may report it twice).
// Streaming tracks are never counted, listed or reported here: the rows are the core's.
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Globalization;
using RbStemsPlus.Core;

namespace RbStemsPlus.Shell
{
    /// What the Prepare and Status models read from one render: a snapshot and its delta (Snap's
    /// accessors in the app, a fake in the tests).
    internal interface IPrepareSource
    {
        string Str(Field f);
        bool Flag(Field f);
        long I64(Field f);
        double F64(Field f);
        int Rows(Table t);
        ulong ListGen(Table t);
        string CellStr(Column c, int row);
        long CellI64(Column c, int row);
        bool CellFlag(Column c, int row);
        /// no delta (a full reload), or the delta says this table reloads
        bool Reload(Table t);
        int OldRows(Table t);
        int[] Changed(Table t);
    }

    public sealed class OutlineRow : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        private readonly PrepareModel owner;
        private string title = "";
        private bool secondary, open, selected;
        private long kind;

        internal OutlineRow(PrepareModel owner, int row, int parent)
        {
            this.owner = owner;
            Row = row;
            Parent = parent;
        }

        /// the row's index in T_PREP_OUTLINE: its identity within a list_gen
        public int Row { get; }
        public int Parent { get; }
        public ObservableCollection<OutlineRow> Children { get; } = new ObservableCollection<OutlineRow>();
        public string Title { get => title; internal set => SetField(ref title, value, nameof(Title)); }
        /// rekordbox's playlist id (C_OUT_ID: the probe's select, collapse and expand name rows by it)
        public string Id { get; internal set; } = "";
        /// drawn as secondary (can't be prepared)
        public bool Secondary { get => secondary; internal set => SetField(ref secondary, value, nameof(Secondary)); }
        /// 0 folder, 1 playlist, 2 intelligent, 3 another kind (schema.def)
        public long Kind { get => kind; internal set => SetField(ref kind, value, nameof(Kind)); }
        public bool IsFolder => Children.Count > 0;
        public override string ToString() => Title;

        /// The expander. The render sets it inside the echo guard; a TwoWay binding's
        /// target-to-source update (the user's click or key) sends {"ev":"folder"}.
        public bool IsExpanded
        {
            get => open;
            set
            {
                if (open == value) return;
                open = value;
                Raise(nameof(IsExpanded));
                owner.FolderChanged(this, value);
            }
        }

        /// The selection. The render sets it inside the echo guard; a TwoWay binding's
        /// target-to-source update to true (the user's click or key) sends {"ev":"select"}.
        public bool IsSelected
        {
            get => selected;
            set
            {
                if (selected == value) return;
                selected = value;
                Raise(nameof(IsSelected));
                if (value) owner.SelectedByUser(this);
            }
        }

        internal void SetOpenFromCore(bool v) { if (open != v) { open = v; Raise(nameof(IsExpanded)); } }
        internal void SetSelectedFromCore(bool v) { if (selected != v) { selected = v; Raise(nameof(IsSelected)); } }

        private void SetField<T>(ref T f, T v, string name)
        {
            if (EqualityComparer<T>.Default.Equals(f, v)) return;
            f = v;
            Raise(name);
        }

        private void Raise(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    public sealed class TrackRow : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler PropertyChanged;
        private string name = "", result = "", why = "";
        public string Name { get => name; internal set { if (name != value) { name = value; Raise(nameof(Name)); } } }
        public string Result { get => result; internal set { if (result != value) { result = value; Raise(nameof(Result)); } } }
        public string Why { get => why; internal set { if (why != value) { why = value; Raise(nameof(Why)); } } }
        /// What Narrator reads for the row (app.md §4.7): its cells as shown, the empty ones left out
        public string Spoken => Cells.Join(name, result, why);
        public override string ToString() => Spoken;
        private void Raise(string n)
        {
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(n));
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(Spoken)));
        }
    }

    /// A row's cells as one accessible name (app.md §4.7): the snapshot's texts, in column order,
    /// the empty ones left out, joined as a list is read.
    public static class Cells
    {
        public static string Join(params string[] cells)
        {
            var shown = new List<string>();
            foreach (var c in cells) if (!string.IsNullOrEmpty(c)) shown.Add(c);
            return string.Join(", ", shown);
        }
    }

    /// The "Keep the stems until" picker's glue, WPF-free (the view lends it the DatePicker's
    /// SelectedDate, its range and the dispatcher): the picker's own changes reach the model only
    /// when the render isn't setting them, a cleared field draws the core's day again later, and
    /// the range follows the day (tomorrow to a year on, as the core checks it).
    public sealed class UntilPicker
    {
        private readonly PrepareModel model;
        private readonly Func<DateTime?> get;
        private readonly Action<DateTime?> set;
        private readonly Action<DateTime, DateTime> range;
        private readonly Action<Action> later;
        private bool setting;
        private DateTime boundsDay;
        /// today (the tests' clock)
        public Func<DateTime> Today = () => DateTime.Today;

        public UntilPicker(PrepareModel model, Func<DateTime?> get, Action<DateTime?> set, Action<DateTime, DateTime> range, Action<Action> later)
        {
            this.model = model;
            this.get = get;
            this.set = set;
            this.range = range;
            this.later = later;
        }

        /// SelectedDateChanged: the user's day, unless the render is setting it
        public void Changed()
        {
            if (setting) return;
            DateTime? d = get();
            if (d == null) { later(Draw); return; }   // a cleared field (Delete): the core's day again, after this change
            model.UntilPicked(d.Value);
        }

        /// the core's day and the range from today; the picker's events meanwhile are the render's
        public void Draw()
        {
            setting = true;
            try
            {
                DateTime today = Today();
                if (boundsDay != today)
                {
                    boundsDay = today;
                    range(today.AddDays(1), today.AddDays(365));
                }
                DateTime? d = PrepareModel.Day(model.Until);
                if (d != null && get() != d) set(d);
            }
            finally { setting = false; }
        }
    }

    public sealed class PrepareModel : INotifyPropertyChanged
    {
        /// the outline's row with rekordbox's id, or null
        public OutlineRow OutlineById(string id)
        {
            foreach (var r in byRow) if (r.Id == id) return r;
            return null;
        }

        /// the outline's row with rekordbox's id among the rows shown (each ancestor open: the
        /// rows a click can reach, the probe's select, collapse and expand), or null
        public OutlineRow ShownOutlineById(string id)
        {
            var r = OutlineById(id);
            for (var a = r == null ? null : OutlineAt(r.Parent); a != null; a = OutlineAt(a.Parent))
                if (!a.IsExpanded) return null;
            return r;
        }

        public event PropertyChangedEventHandler PropertyChanged;
        private readonly Action<string> send;
        private readonly Dictionary<string, object> values = new Dictionary<string, object>();
        private readonly List<OutlineRow> byRow = new List<OutlineRow>();
        private ulong outlineGen = ulong.MaxValue, tracksGen = ulong.MaxValue;
        private long scrollSeq, outlineScrollSeq, focusSeq;
        private bool applying;
        private int shownSelection = -1;

        /// send: one event to the core ({"ev":...} JSON), from the UI thread, outside a render.
        public PrepareModel(Action<string> send) { this.send = send; }

        public ObservableCollection<OutlineRow> Roots { get; } = new ObservableCollection<OutlineRow>();
        public ObservableCollection<TrackRow> Tracks { get; } = new ObservableCollection<TrackRow>();

        /// one-shots for the view, raised during Apply: the tracks' row to scroll into view, the
        /// outline row to scroll into view, the outline to take the keyboard focus
        public event Action<int> ScrollTracksTo;
        /// the probe's (TEST builds): once per render that changed a list, as the Mac shell's
        /// reloadData (whole: the row count) or reloadData(forRowIndexes:) (the first changed row)
        public Action<bool, int> TracksRendered, OutlineRendered;
        public event Action<OutlineRow> ScrollOutlineTo;
        public event Action FocusOutline;
        /// after every render's S_PREPARE pass, changed or not: the view draws again what a
        /// control may show differently from the model (the date picker after a refused day, its
        /// range after midnight)
        public event Action Applied;

        private T Get<T>(string name, T fallback = default(T)) => values.TryGetValue(name, out var v) ? (T)v : fallback;
        private void Set<T>(string name, T value)
        {
            if (values.TryGetValue(name, out var old) && Equals(old, value)) return;
            values[name] = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
        }

        /// the playlist's name ("Choose a playlist" until the core names one, as the Mac's label)
        public string Name => Get(nameof(Name), "");
        public string Info => Get(nameof(Info), "");
        public string InfoSub => Get(nameof(InfoSub), "");
        public string PrepareTitle => Get(nameof(PrepareTitle), "");
        public bool PrepareEnabled => Get<bool>(nameof(PrepareEnabled));
        public bool CheckEnabled => Get<bool>(nameof(CheckEnabled));
        /// "Keep the stems until", YYYY-MM-DD local ("" until the core says)
        public string Until => Get(nameof(Until), "");
        public bool UntilEnabled => Get<bool>(nameof(UntilEnabled));
        public bool Running => Get<bool>(nameof(Running));
        public string PauseTitle => Get(nameof(PauseTitle), "");
        public double Progress => Get<double>(nameof(Progress));
        public bool OutlineEnabled => Get<bool>(nameof(OutlineEnabled));
        /// the outline row the core has selected, or -1
        public int SelectedRow => Get(nameof(SelectedRow), -1);
        public bool NotRunning => !Running;
        public int OutlineCount => byRow.Count;
        public OutlineRow OutlineAt(int row) => row >= 0 && row < byRow.Count ? byRow[row] : null;

        /// One render's S_PREPARE pass (main, inside render: accessors only, no event sent).
        internal void Apply(IPrepareSource s)
        {
            applying = true;
            untilSent = null;   // a render: the core's answer to the day sent (accepted, or drawn back)
            try
            {
                bool wasRunning = Running;
                Set(nameof(Name), s.Str(Field.F_PREP_NAME));
                Set(nameof(Info), s.Str(Field.F_PREP_INFO));
                Set(nameof(InfoSub), s.Str(Field.F_PREP_INFO_SUB));
                Set(nameof(PrepareTitle), s.Str(Field.F_PREP_PREPARE_TITLE));
                Set(nameof(PrepareEnabled), s.Flag(Field.F_PREP_PREPARE_ENABLED));
                Set(nameof(CheckEnabled), s.Flag(Field.F_PREP_CHECK_ENABLED));
                Set(nameof(Until), s.Str(Field.F_PREP_UNTIL));
                Set(nameof(UntilEnabled), s.Flag(Field.F_PREP_UNTIL_ENABLED));
                Set(nameof(Running), s.Flag(Field.F_PREP_RUNNING));
                if (wasRunning != Running) PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(NotRunning)));
                Set(nameof(PauseTitle), s.Str(Field.F_PREP_PAUSE_TITLE));
                Set(nameof(Progress), s.F64(Field.F_PREP_PROGRESS));
                Set(nameof(OutlineEnabled), s.Flag(Field.F_PREP_OUTLINE_ENABLED));
                ApplyOutline(s);
                ApplyTracks(s);
                ApplyOneShots(s);
                Applied?.Invoke();
            }
            finally { applying = false; }
        }

        private void ApplyOutline(IPrepareSource s)
        {
            int n = s.Rows(Table.T_PREP_OUTLINE);
            ulong gen = s.ListGen(Table.T_PREP_OUTLINE);
            bool reload = s.Reload(Table.T_PREP_OUTLINE) || gen != outlineGen || s.OldRows(Table.T_PREP_OUTLINE) != n || n != byRow.Count;
            outlineGen = gen;
            if (reload)
            {
                Roots.Clear();
                byRow.Clear();
                for (int r = 0; r < n; r++)
                {
                    int p = (int)s.CellI64(Column.C_OUT_PARENT, r);
                    var row = new OutlineRow(this, r, p >= 0 && p < r ? p : -1);
                    Fill(row, s);
                    byRow.Add(row);
                    if (row.Parent >= 0) byRow[row.Parent].Children.Add(row); else Roots.Add(row);
                }
                foreach (var row in byRow) row.SetOpenFromCore(row.IsFolder && s.CellFlag(Column.C_OUT_OPEN, row.Row));
                OutlineRendered?.Invoke(true, n);
            }
            else
            {
                foreach (int r in s.Changed(Table.T_PREP_OUTLINE))
                {
                    if (r < 0 || r >= byRow.Count) continue;
                    Fill(byRow[r], s);
                    byRow[r].SetOpenFromCore(byRow[r].IsFolder && s.CellFlag(Column.C_OUT_OPEN, r));
                }
            }
            int sel = (int)s.I64(Field.F_PREP_SELECTED_ROW);
            if (sel < 0 || sel >= byRow.Count) sel = -1;
            Set(nameof(SelectedRow), sel);
            shownSelection = sel;
            for (int r = 0; r < byRow.Count; r++) byRow[r].SetSelectedFromCore(r == sel);
        }

        private static void Fill(OutlineRow row, IPrepareSource s)
        {
            row.Title = s.CellStr(Column.C_OUT_TITLE, row.Row);
            row.Id = s.CellStr(Column.C_OUT_ID, row.Row);
            row.Secondary = s.CellFlag(Column.C_OUT_SECONDARY, row.Row);
            row.Kind = s.CellI64(Column.C_OUT_KIND, row.Row);
        }

        private void ApplyTracks(IPrepareSource s)
        {
            int n = s.Rows(Table.T_PREP_TRACKS);
            ulong gen = s.ListGen(Table.T_PREP_TRACKS);
            if (s.Reload(Table.T_PREP_TRACKS) || gen != tracksGen || n < Tracks.Count || (Tracks.Count == 0 && n > 0))
            {
                tracksGen = gen;
                Tracks.Clear();
                for (int r = 0; r < n; r++) Tracks.Add(Track(s, r, new TrackRow()));
                TracksRendered?.Invoke(true, n);
                return;
            }
            int[] changed = s.Changed(Table.T_PREP_TRACKS);
            foreach (int r in changed) if (r >= 0 && r < Tracks.Count) Track(s, r, Tracks[r]);
            if (changed.Length > 0) TracksRendered?.Invoke(false, changed[0]);
            for (int r = Tracks.Count; r < n; r++) Tracks.Add(Track(s, r, new TrackRow()));   // the estimate's rows as they come
        }

        private static TrackRow Track(IPrepareSource s, int r, TrackRow t)
        {
            t.Name = s.CellStr(Column.C_TRK_NAME, r);
            t.Result = s.CellStr(Column.C_TRK_RESULT, r);
            t.Why = s.CellStr(Column.C_TRK_WHY, r);
            return t;
        }

        // each once per new value (0 at first, as the Mac shell's); the view keeps a request it can't
        // carry out yet (the tab not loaded) until it can (SO13)
        private void ApplyOneShots(IPrepareSource s)
        {
            long seq = s.I64(Field.F_PREP_SCROLL_SEQ);
            if (seq != scrollSeq)
            {
                scrollSeq = seq;
                int r = (int)s.I64(Field.F_PREP_SCROLL_ROW);
                if (r >= 0 && r < Tracks.Count) ScrollTracksTo?.Invoke(r);
            }
            long os = s.I64(Field.F_PREP_OUTLINE_SCROLL_SEQ);
            if (os != outlineScrollSeq)
            {
                outlineScrollSeq = os;
                var row = OutlineAt(SelectedRow);
                if (row != null) ScrollOutlineTo?.Invoke(row);
            }
            long f = s.I64(Field.F_PREP_FOCUS_SEQ);
            if (f != focusSeq)
            {
                focusSeq = f;
                FocusOutline?.Invoke();
            }
        }

        // ------------------------------------------------------------------ user intent

        internal void FolderChanged(OutlineRow row, bool open)
        {
            if (applying || !row.IsFolder) return;   // the render's own expansion: no echo; a leaf has none
            send("{\"ev\":\"folder\",\"row\":" + Json.Num(row.Row) + ",\"open\":" + (open ? "true" : "false") + ",\"gen\":" + Json.Num(outlineGen) + "}");
        }

        internal void SelectedByUser(OutlineRow row)
        {
            if (applying || row.Row == shownSelection) return;   // the render's own, or the one shown again
            shownSelection = row.Row;
            send("{\"ev\":\"select\",\"table\":\"T_PREP_OUTLINE\",\"row\":" + Json.Num(row.Row) + ",\"gen\":" + Json.Num(outlineGen) + "}");
        }

        /// The date picker's new day (the user's): {"ev":"until"}. A day equal to the core's sends nothing.
        /// The day sent and not answered yet: WPF's DatePicker raises SelectedDateChanged again for
        /// one change (a day past DisplayDateEnd: once for the day, once more after its own
        /// coercion), so the same day again sends nothing until the next render, the core's answer.
        private string untilSent;

        public void UntilPicked(DateTime day)
        {
            if (applying) return;
            string d = day.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
            if (d == Until || d == untilSent) return;
            untilSent = d;
            send("{\"ev\":\"until\",\"date\":" + Json.Quote(d) + "}");
        }

        /// The buttons: Prepare (or Keep), Check Again, Pause/Resume, Stop (flows.c's click ids).
        public void Click(string id)
        {
            if (applying) return;
            send(Json.Click(id));
        }

        /// The core's date as a day ("" or malformed: null).
        public static DateTime? Day(string yyyyMmDd) =>
            DateTime.TryParseExact(yyyyMmDd, "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out DateTime d) ? d : (DateTime?)null;
    }
}
