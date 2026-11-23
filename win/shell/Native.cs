// core.h API v1 as the WPF shell sees it (app.md §4.1): one DllImport("rbsp-core.dll") set, bound
// to the module Program preloads from x64\ or arm64\ (W0: the preload binds). The ABI rules of
// core.h's head: cdecl, size_t as UIntPtr, truth values as int, returned strings as IntPtr + length
// (never a MarshalAs return), strings in as LPUTF8Str. No WPF here: the Mac tests (tests/) compile
// this file for .NET on macOS against the core built as a dylib.
using System;
using System.Runtime.InteropServices;
using System.Security;

namespace RbStemsPlus.Shell
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct CoreHost
    {
        public uint size;
        public uint reserved;
        public IntPtr ctx;
        public IntPtr render, front_facts, modal_open, facts, post_main, post_work, dialog, pick_folder,
            slow_fact, helper_spawn, helper_signal, admin, step, close_reply, log;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CoreMenuItem
    {
        public int id, menu, kind, title;
        public IntPtr key;
        public uint mods, reserved;
        public IntPtr evt;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CoreSlot
    {
        public int id, reserved;
        public IntPtr value;
        public UIntPtr len;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct CoreMaskCtx
    {
        public uint size, reserved;
        public IntPtr home, names;
        public int n_names, n_allowed;
        public IntPtr allowed;
    }

    // the host table's entries (core.h "Host callbacks"); cdecl, the only convention on x64/arm64
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void RenderFn(IntPtr ctx, IntPtr snap, IntPtr delta);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate uint FrontFactsFn(IntPtr ctx);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int ModalOpenFn(IntPtr ctx);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void FactsFn(IntPtr ctx, IntPtr sheet);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void PostMainFn(IntPtr ctx, IntPtr job);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void PostWorkFn(IntPtr ctx, int kind, IntPtr job);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int DialogFn(IntPtr ctx, IntPtr d, IntPtr r);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int PickFolderFn(IntPtr ctx, IntPtr start, UIntPtr len, IntPtr r);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void CloseReplyFn(IntPtr ctx, int what, int proceed);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void LogFn(IntPtr ctx, IntPtr line, UIntPtr len);

    [SuppressUnmanagedCodeSecurity]
    internal static unsafe class Core
    {
        private const string Dll = "rbsp-core.dll";
        private const CallingConvention C = CallingConvention.Cdecl;

        // the layouts core.h freezes (W0 checked them; the start-up checks them again)
        public const int HostSize = 136, MenuItemSize = 40, SlotSize = 24, MaskCtxSize = 40;

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern uint core_api_version();
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_ids_hash();
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_selftest();

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_window_save([MarshalAs(UnmanagedType.LPUTF8Str)] string parent,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string support, [MarshalAs(UnmanagedType.LPUTF8Str)] string name, byte[] text, UIntPtr len);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_win_log_append([MarshalAs(UnmanagedType.LPUTF8Str)] string parent,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string support, byte[] text, UIntPtr len);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern IntPtr core_new(ref CoreHost host, int role, [MarshalAs(UnmanagedType.LPUTF8Str)] string config, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_free(IntPtr c);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_run(IntPtr job);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_job_drop(IntPtr job);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_commit(IntPtr c);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_event(IntPtr c, [MarshalAs(UnmanagedType.LPUTF8Str)] string json, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_close_asked(IntPtr c, int what);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_snap_retain(IntPtr s);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_snap_release(IntPtr s);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_snap_rev(IntPtr s, int section);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern IntPtr core_str(IntPtr s, int f, out UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_flag(IntPtr s, int f);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern long core_i64(IntPtr s, int f);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern double core_f64(IntPtr s, int f);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_rows(IntPtr s, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_list_gen(IntPtr s, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern IntPtr core_cell_str(IntPtr s, int col, int row, out UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_cell_flag(IntPtr s, int col, int row);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern long core_cell_i64(IntPtr s, int col, int row);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern uint core_delta_sections(IntPtr d);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_delta_reload(IntPtr d, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_delta_old_rows(IntPtr d, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int* core_delta_changed(IntPtr d, int t, out int n);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern IntPtr core_dlg_text(IntPtr d, int what, int i, int j, out UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_dlg_int(IntPtr d, int what, int i, int j);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_reply_button(IntPtr r, int i);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_reply_row_button(IntPtr r, int row, int j);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern void core_reply_field(IntPtr r, int i, [MarshalAs(UnmanagedType.LPUTF8Str)] string v, UIntPtr len);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_menu_count(int os);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_menu_at(int os, int i, out CoreMenuItem item);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern uint core_menu_state(IntPtr c, int id, out IntPtr title, out UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_text(int os, int id, CoreSlot* slots, int nslots, byte* buf, UIntPtr cap, out UIntPtr len);

        // CORE_TEST_HOOKS (a TEST DLL only; never called by the app, bound only when a test calls one)
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_test_set_str(IntPtr c, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_test_set_i64(IntPtr c, int f, long v);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_test_flow(IntPtr c, int mode);

        public static string Utf8(IntPtr p, UIntPtr len) =>
            p == IntPtr.Zero ? "" : System.Text.Encoding.UTF8.GetString((byte*)p, checked((int)len.ToUInt64()));

        public static string Utf8z(IntPtr p)
        {
            if (p == IntPtr.Zero) return "";
            int n = 0;
            while (((byte*)p)[n] != 0) n++;
            return System.Text.Encoding.UTF8.GetString((byte*)p, n);
        }
    }

    /// core_new's config (core.h): the OS, the version, the paths of app.md §5.10 the shell knows,
    /// and the entries the DLL runs itself. Nothing rooted in Program Files is computed or written
    /// here (PF3): the DLL derives rekordbox's folder and ours from its one Program Files rule (the
    /// prepare helper's --rekordbox and --ort are its own whatever the config says), so "rekordbox"
    /// and "root_ort" stay out. "model" is rekordbox's stems model under the account's roaming
    /// AppData (Paths.swift's modelDir + "/hdemucs.onnx", models.h's folder): models.c's restore,
    /// copy and look at originals\ and the facts of host_facts.c read it. Without a roaming folder
    /// there is no "model", and the model steps fail closed (an uninstall stops at the restore).
    /// "helper" is the prepare helper beside this exe in the version folder (the zip's
    /// rbsp-prepare.exe, release.md §3; the Mac's Contents/Helpers/rbsp-prepare): the DLL spawns it,
    /// and FACT_HELPER_THERE says whether it is there. "kept"
    /// is support\kept (helper.md §1, the Mac's keptDir): the helper's --kept-dir, the Status tab's
    /// kept sets and Stop Keeping.
    internal static class CoreConfig
    {
        public const string HelperName = "rbsp-prepare.exe";

        // The window and the core use the same setting. TEST builds can exercise both layouts.
#if SHELL_TEST
        internal static readonly bool PrepareShown = TestHooks.Has("prepare");
#else
        internal static readonly bool PrepareShown = true;
#endif

        public static string RekordboxModel(string roaming) =>
            string.IsNullOrEmpty(roaming) ? null : System.IO.Path.Combine(roaming, "Pioneer", "rekordbox6", "models", "demucs3_model", "hdemucs.onnx");

        public static string Build(string version, string home, string support, string app, string roaming, string tmp, string native)
        {
            string P(string k, string v) => Json.Quote(k) + ":" + Json.Quote(v);
            string model = RekordboxModel(roaming);
            string paths = string.Join(",",
                P("home", home),
                P("support", support),
                P("app", app),
                P("helper", System.IO.Path.Combine(app, HelperName)),
                P("kept", System.IO.Path.Combine(support, "kept")),
                P("log", System.IO.Path.Combine(support, "Logs")),
                P("cache_default", System.IO.Path.Combine(support, "Cache")),
                P("tmp", tmp));
            if (model != null) paths += "," + P("model", model);
            string tabs = PrepareShown ? ",\"prepare_tabs\":true" : "";
            return "{\"os\":\"win\",\"version\":" + Json.Quote(version) + ",\"paths\":{" + paths + "},\"native\":" + native + tabs + "}";
        }
    }
}
