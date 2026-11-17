// The window renders snapshots and holds view handles only (app.md §4.2): the status line, the
// track rows (rebuilt on a new list_gen, changed in place by the delta's rows, appended past
// old_rows) and the menu bar drawn from menus.def.
using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Windows;
using System.Windows.Controls;
using RbStemsPlus.Core;

namespace RbStemsPlus.W0
{
    public sealed class TrackRow : INotifyPropertyChanged
    {
        private string name, result;
        public string Name { get => name; set { if (name != value) { name = value; Changed(nameof(Name)); } } }
        public string Result { get => result; set { if (result != value) { result = value; Changed(nameof(Result)); } } }
        public event PropertyChangedEventHandler PropertyChanged;
        private void Changed(string p) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(p));
    }

    public partial class MainWindow : Window
    {
        private ulong listGen = ulong.MaxValue;
        public long FirstRowsDrawnTicks;

        public MainWindow()
        {
            InitializeComponent();
            rowsReplace(new ObservableCollection<TrackRow>());
        }

        internal unsafe void Apply(IntPtr snap, IntPtr delta)
        {
            uint sections = delta == IntPtr.Zero ? uint.MaxValue : Core.core_delta_sections(delta);
            if ((sections & (1u << (int)Section.S_APP)) != 0)
            {
                StatusLine.Text = Str(snap, Field.F_STATUS_LINE);
                string title = Str(snap, Field.F_WINDOW_TITLE);
                if (!string.IsNullOrEmpty(title)) Title = title;
            }
            if ((sections & (1u << (int)Section.S_PREPARE)) == 0) return;
            int t = (int)Table.T_PREP_TRACKS;
            int n = Core.core_rows(snap, t);
            ulong gen = Core.core_list_gen(snap, t);
            if (delta == IntPtr.Zero || gen != listGen || Core.core_delta_reload(delta, t) != 0)
            {
                // a new list_gen: reload everything, in one assignment (no per-row CollectionChanged)
                listGen = gen;
                var fresh = new ObservableCollection<TrackRow>();
                for (int i = 0; i < n; i++) fresh.Add(Row(snap, i));
                rowsReplace(fresh);
            }
            else
            {
                int* changed = Core.core_delta_changed(delta, t, out int k);
                for (int j = 0; j < k; j++)
                {
                    int i = changed[j];
                    var r = current[i];
                    r.Name = Cell(snap, Column.C_TRK_NAME, i);
                    r.Result = Cell(snap, Column.C_TRK_RESULT, i);
                }
                for (int i = Core.core_delta_old_rows(delta, t); i < n; i++) current.Add(Row(snap, i));
            }
            Footer.Text = n + " rows";
        }

        private ObservableCollection<TrackRow> current;

        private void rowsReplace(ObservableCollection<TrackRow> fresh)
        {
            current = fresh;
            Tracks.ItemsSource = fresh;
        }

        private static TrackRow Row(IntPtr snap, int i) =>
            new TrackRow { Name = Cell(snap, Column.C_TRK_NAME, i), Result = Cell(snap, Column.C_TRK_RESULT, i) };

        internal static string Str(IntPtr snap, Field f)
        {
            IntPtr p = Core.core_str(snap, (int)f, out UIntPtr len);
            return Core.Utf8(p, len);
        }

        internal static string Cell(IntPtr snap, Column c, int row)
        {
            IntPtr p = Core.core_cell_str(snap, (int)c, row, out UIntPtr len);
            return Core.Utf8(p, len);
        }

        // the menu bar from menus.def (core_menu_count/_at for Windows, titles from core_text)
        internal unsafe int BuildMenus()
        {
            MenuBar.Items.Clear();
            var items = new Dictionary<int, MenuItem>();
            int count = Core.core_menu_count(CoreApi.CORE_OS_WIN);
            byte* buf = stackalloc byte[256];
            for (int i = 0; i < count; i++)
            {
                if (Core.core_menu_at(CoreApi.CORE_OS_WIN, i, out CoreMenuItem m) != CoreApi.CORE_OK) continue;
                if (m.kind == 2 /* CORE_MENU_NATIVE */) continue;
                string title = "";
                if (m.title >= 0 && Core.core_text(CoreApi.CORE_OS_WIN, m.title, null, 0, buf, (UIntPtr)256, out UIntPtr len) == CoreApi.CORE_OK)
                    title = System.Text.Encoding.UTF8.GetString(buf, (int)len.ToUInt64());
                ItemsControl parent = m.menu < 0 ? (ItemsControl)MenuBar : (items.TryGetValue(m.menu, out var p) ? p : null);
                if (parent == null) continue;
                if (m.kind == 1) { parent.Items.Add(new Separator()); continue; }
                var mi = new MenuItem { Header = title.Replace("&", "_") };
                int id = m.id;
                mi.SubmenuOpened += (s, e) => RefreshStates((MenuItem)s);
                items[m.id] = mi;
                mi.Tag = id;
                parent.Items.Add(mi);
            }
            return count;
        }

        private void RefreshStates(MenuItem parent)
        {
            foreach (var o in parent.Items)
            {
                if (!(o is MenuItem mi) || !(mi.Tag is int id)) continue;
                uint st = Core.core_menu_state(Host.C, id, out IntPtr title, out UIntPtr len);
                mi.IsEnabled = (st & 1u) != 0;
                mi.Visibility = (st & 2u) != 0 ? Visibility.Collapsed : Visibility.Visible;
                mi.IsChecked = (st & 4u) != 0;
                if (title != IntPtr.Zero) mi.Header = Core.Utf8(title, len);
            }
        }
    }
}
