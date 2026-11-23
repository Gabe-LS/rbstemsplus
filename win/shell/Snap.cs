// Typed accessors over a snapshot (app.md §4.1), by the generated ids' names (CoreIds.cs, made from
// core's schema.def, texts.def and facts.def at build time): no number is written by hand, so a
// schema row added at G4 changes nothing here but the generated file. Strings stay valid while the
// snapshot is held (core.h Snapshots); they are copied out as .NET strings at once.
using System;
using System.Text;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    internal static unsafe class Snap
    {
        public static string Str(IntPtr s, Field f) => Core.Utf8(Core.core_str(s, (int)f, out UIntPtr n), n);
        public static bool Flag(IntPtr s, Field f) => Core.core_flag(s, (int)f) != 0;
        public static long I64(IntPtr s, Field f) => Core.core_i64(s, (int)f);
        public static double F64(IntPtr s, Field f) => Core.core_f64(s, (int)f);
        public static int Rows(IntPtr s, Table t) => Core.core_rows(s, (int)t);
        public static ulong ListGen(IntPtr s, Table t) => Core.core_list_gen(s, (int)t);
        public static string Cell(IntPtr s, Column c, int row) => Core.Utf8(Core.core_cell_str(s, (int)c, row, out UIntPtr n), n);

        public static bool CellFlag(IntPtr s, Column c, int row) => Core.core_cell_flag(s, (int)c, row) != 0;
        public static long CellI64(IntPtr s, Column c, int row) => Core.core_cell_i64(s, (int)c, row);

        /// The sections whose revision moved: all of them for a full reload (no delta).
        public static uint Moved(IntPtr delta) => delta == IntPtr.Zero ? uint.MaxValue : Core.core_delta_sections(delta);
        public static bool Has(uint moved, Section s) => (moved & (1u << (int)s)) != 0;
    }

    /// One render's snapshot and delta as the Prepare and Status tabs' models read them (no delta: a full reload).
    internal sealed unsafe class SnapPrepareSource : IPrepareSource
    {
        private readonly IntPtr s, d;
        public SnapPrepareSource(IntPtr snap, IntPtr delta) { s = snap; d = delta; }
        public string Str(Field f) => Snap.Str(s, f);
        public bool Flag(Field f) => Snap.Flag(s, f);
        public long I64(Field f) => Snap.I64(s, f);
        public double F64(Field f) => Snap.F64(s, f);
        public int Rows(Table t) => Snap.Rows(s, t);
        public ulong ListGen(Table t) => Snap.ListGen(s, t);
        public string CellStr(Column c, int row) => Snap.Cell(s, c, row);
        public long CellI64(Column c, int row) => Snap.CellI64(s, c, row);
        public bool CellFlag(Column c, int row) => Snap.CellFlag(s, c, row);
        public bool Reload(Table t) => d == IntPtr.Zero || Core.core_delta_reload(d, (int)t) != 0;
        public int OldRows(Table t) => d == IntPtr.Zero ? -1 : Core.core_delta_old_rows(d, (int)t);
        public int[] Changed(Table t)
        {
            if (d == IntPtr.Zero) return new int[0];
            int* p = Core.core_delta_changed(d, (int)t, out int n);
            var r = new int[p == null || n < 0 ? 0 : n];
            for (int i = 0; i < r.Length; i++) r[i] = p[i];
            return r;
        }
    }

    /// The core's texts (core_text, a pure function: any thread), the Windows column.
    internal static unsafe class Texts
    {
        public static string Get(CoreText id) => Get(id, null);

        /// A text with its slots filled ("" when the core refuses: an unknown slot, a Mac-only text).
        public static string Get(CoreText id, params (Slot slot, string value)[] slots)
        {
            int n = slots == null ? 0 : slots.Length;
            var bytes = new byte[n][];
            for (int i = 0; i < n; i++) bytes[i] = Encoding.UTF8.GetBytes(slots[i].value ?? "");
            CoreSlot* sl = stackalloc CoreSlot[Math.Max(n, 1)];
            var pins = new System.Runtime.InteropServices.GCHandle[n];
            try
            {
                for (int i = 0; i < n; i++)
                {
                    pins[i] = System.Runtime.InteropServices.GCHandle.Alloc(bytes[i], System.Runtime.InteropServices.GCHandleType.Pinned);
                    sl[i] = new CoreSlot { id = (int)slots[i].slot, reserved = 0, value = pins[i].AddrOfPinnedObject(), len = (UIntPtr)bytes[i].Length };
                }
                int cap = 1024;
                for (int attempt = 0; attempt < 2; attempt++)   // the second with the full length (CORE_E_SPACE)
                {
                    var buf = new byte[cap];
                    fixed (byte* b = buf)
                    {
                        int r = Core.core_text(CoreApi.CORE_OS_WIN, (int)id, n == 0 ? null : sl, n, b, (UIntPtr)cap, out UIntPtr len);
                        if (r == CoreApi.CORE_OK) return Encoding.UTF8.GetString(b, (int)len.ToUInt64());
                        if (r != CoreApi.CORE_E_SPACE) return "";
                        cap = checked((int)len.ToUInt64() + 1);
                    }
                }
                return "";
            }
            finally
            {
                for (int i = 0; i < n; i++) if (pins[i].IsAllocated) pins[i].Free();
            }
        }

        /// A menu title's words outside the menu (a button, a window title, an automation name):
        /// its access key's "_" dropped as WPF's AccessText reads it ("__" is one "_").
        public static string Plain(CoreText id) => StripAccessKey(Get(id));

        public static string StripAccessKey(string s)
        {
            var b = new StringBuilder(s.Length);
            for (int i = 0; i < s.Length; i++)
            {
                if (s[i] == '_' && i + 1 < s.Length) i++;
                b.Append(s[i]);
            }
            return b.ToString();
        }
    }

    /// JSON for the low-rate events (core_event): the shell writes it, never reads it (app.md §4.1).
    internal static class Json
    {
        public static string Quote(string s)
        {
            var o = new StringBuilder(s.Length + 2).Append('"');
            foreach (char ch in s)
            {
                switch (ch)
                {
                    case '"': o.Append("\\\""); break;
                    case '\\': o.Append("\\\\"); break;
                    case '\n': o.Append("\\n"); break;
                    case '\r': o.Append("\\r"); break;
                    case '\t': o.Append("\\t"); break;
                    default:
                        if (ch < 0x20) o.Append("\\u").Append(((int)ch).ToString("x4"));
                        else o.Append(ch);
                        break;
                }
            }
            return o.Append('"').ToString();
        }

        /// a JSON number: never the user's culture (its minus sign may not be '-')
        public static string Num(long n) => n.ToString(System.Globalization.CultureInfo.InvariantCulture);
        public static string Num(ulong n) => n.ToString(System.Globalization.CultureInfo.InvariantCulture);

        public static string Click(string id) => "{\"ev\":\"click\",\"id\":" + Quote(id) + "}";
        /// R1's Don't Ask Again: the versions its button showed when clicked (ManageModel.R1DismissVersions)
        public static string R1Dismiss(string versions) => "{\"ev\":\"r1_dismiss\",\"versions\":" + Quote(versions ?? "") + "}";
        public static string Menu(string id) => "{\"ev\":\"menu\",\"id\":" + Quote(id) + "}";
        public static string Tab(int tab) => "{\"ev\":\"tab\",\"tab\":" + Num(tab) + "}";
    }
}
