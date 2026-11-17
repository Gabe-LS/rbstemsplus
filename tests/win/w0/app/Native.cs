// core.h API v1 as the WPF shell sees it (app.md §4.1): one DllImport("rbsp-core.dll") set, bound
// to the module Program preloads from x64\ or arm64\. The ABI rules of core.h's head: cdecl,
// size_t as UIntPtr, truth values as int, returned strings as IntPtr + length (never a MarshalAs
// return), strings in as LPUTF8Str (G1.10) or byte* + length (its fallback, also declared here).
using System;
using System.Runtime.InteropServices;
using System.Security;

namespace RbStemsPlus.W0
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
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int SlowFactFn(IntPtr ctx, int kind, IntPtr key, UIntPtr len, IntPtr sheet);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int HelperSpawnFn(IntPtr ctx, ulong run, IntPtr argv, int argc);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void HelperSignalFn(IntPtr ctx, ulong run, int sig);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int AdminFn(IntPtr ctx, int kind, IntPtr version, IntPtr bundle, IntPtr bridgeSha, IntPtr ortSha, int force);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate int StepFn(IntPtr ctx, int kind, IntPtr args, int nargs, IntPtr sheet);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void CloseReplyFn(IntPtr ctx, int what, int proceed);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] internal delegate void LogFn(IntPtr ctx, IntPtr line, UIntPtr len);

    [SuppressUnmanagedCodeSecurity]
    internal static unsafe class Core
    {
        private const string Dll = "rbsp-core.dll";
        private const CallingConvention C = CallingConvention.Cdecl;

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern uint core_api_version();
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_ids_hash();
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_selftest();

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern IntPtr core_new(ref CoreHost host, int role, [MarshalAs(UnmanagedType.LPUTF8Str)] string config, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_free(IntPtr c);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_run(IntPtr job);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_job_drop(IntPtr job);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_commit(IntPtr c);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_event(IntPtr c, [MarshalAs(UnmanagedType.LPUTF8Str)] string json, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true, EntryPoint = "core_event")]
        public static extern int core_event_b(IntPtr c, byte* json, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_close_asked(IntPtr c, int what);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_helper_line(IntPtr c, ulong run, byte* line, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_helper_ended(IntPtr c, ulong run, int status, byte* err, UIntPtr len);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern IntPtr core_snap_current(IntPtr c);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_snap_retain(IntPtr s);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_snap_release(IntPtr s);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_snap_rev(IntPtr s, int section);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern IntPtr core_str(IntPtr s, int f, out UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_flag(IntPtr s, int f);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern long core_i64(IntPtr s, int f);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern double core_f64(IntPtr s, int f);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_rows(IntPtr s, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_list_gen(IntPtr s, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_row_rev(IntPtr s, int t, int row);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern IntPtr core_cell_str(IntPtr s, int col, int row, out UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_cell_flag(IntPtr s, int col, int row);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern long core_cell_i64(IntPtr s, int col, int row);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern double core_cell_f64(IntPtr s, int col, int row);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern uint core_delta_sections(IntPtr d);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_delta_reload(IntPtr d, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_delta_old_rows(IntPtr d, int t);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int* core_delta_changed(IntPtr d, int t, out int n);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_snap_dump(IntPtr s, byte* buf, UIntPtr cap, out UIntPtr len);

        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_fact_flag(IntPtr sheet, int f, int v);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern void core_fact_i64(IntPtr sheet, int f, long v);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern void core_fact_str(IntPtr sheet, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v, UIntPtr len);

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

        // CORE_TEST_HOOKS (the test DLL only)
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_test_set_str(IntPtr c, int f, [MarshalAs(UnmanagedType.LPUTF8Str)] string v, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true, EntryPoint = "core_test_set_str")]
        public static extern int core_test_set_str_b(IntPtr c, int f, byte* v, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_test_set_i64(IntPtr c, int f, long v);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_test_table_reset(IntPtr c, int t, int rows);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_test_table_append(IntPtr c, int t, int rows);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)]
        public static extern int core_test_cell_str(IntPtr c, int col, int row, [MarshalAs(UnmanagedType.LPUTF8Str)] string v, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true, EntryPoint = "core_test_cell_str")]
        public static extern int core_test_cell_str_b(IntPtr c, int col, int row, byte* v, UIntPtr len);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_test_flow(IntPtr c, int mode);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern int core_test_set_preparing(IntPtr c, int preparing);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern ulong core_test_helper_start(IntPtr c, int purpose);
        [DllImport(Dll, CallingConvention = C, ExactSpelling = true)] public static extern long core_test_helper_lines(IntPtr c, int purpose);

        public static string Utf8(IntPtr p, UIntPtr len) =>
            p == IntPtr.Zero ? null : System.Text.Encoding.UTF8.GetString((byte*)p, checked((int)len.ToUInt64()));
    }

    // the same entry without SuppressUnmanagedCodeSecurity: the per-call cost of the security stack walk
    internal static class CoreChecked
    {
        [DllImport("rbsp-core.dll", CallingConvention = CallingConvention.Cdecl, ExactSpelling = true)] public static extern uint core_api_version();
    }

    [SuppressUnmanagedCodeSecurity]
    internal static unsafe class Win32
    {
        public const uint LOAD_LIBRARY_SEARCH_USER_DIRS = 0x400, LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x800;
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)] public static extern int SetDefaultDllDirectories(uint flags);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern IntPtr GetModuleHandleW(string name);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern uint GetModuleFileNameW(IntPtr module, char* buf, uint size);
        [DllImport("kernel32.dll", ExactSpelling = true)] public static extern uint GetCurrentThreadId();
        [DllImport("kernel32.dll", ExactSpelling = true)] public static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)]
        public static extern int IsWow64Process2(IntPtr process, out ushort processMachine, out ushort nativeMachine);
        [DllImport("psapi.dll", SetLastError = true, ExactSpelling = true)]
        public static extern int EnumProcessModulesEx(IntPtr process, IntPtr* modules, uint cb, out uint needed, uint filter);

        public delegate int EnumWindowsProc(IntPtr hwnd, IntPtr lparam);
        [DllImport("user32.dll", ExactSpelling = true)] public static extern int EnumThreadWindows(uint thread, EnumWindowsProc fn, IntPtr lparam);
        [DllImport("user32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hwnd, char* buf, int max);
        [DllImport("user32.dll", ExactSpelling = true)] public static extern int IsWindowVisible(IntPtr hwnd);
        [DllImport("user32.dll", ExactSpelling = true)] public static extern IntPtr GetWindow(IntPtr hwnd, uint cmd);   // GW_OWNER = 4
        [DllImport("user32.dll", ExactSpelling = true)] public static extern IntPtr PostMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);
        [DllImport("user32.dll", ExactSpelling = true)] public static extern IntPtr SendMessageW(IntPtr hwnd, uint msg, IntPtr w, IntPtr l);

        [DllImport("comctl32.dll", ExactSpelling = true, CharSet = CharSet.Unicode, PreserveSig = true)]
        public static extern int TaskDialog(IntPtr owner, IntPtr inst, string title, string instruction, string content, int buttons, IntPtr icon, out int pressed);
        public const int TDCBF_OK_BUTTON = 1, TDCBF_CANCEL_BUTTON = 8, IDOK = 1, IDCANCEL = 2;
        public const uint WM_COMMAND = 0x0111, WM_USER = 0x0400, TDM_CLICK_BUTTON = WM_USER + 102;
    }

    // IFileOpenDialog, from Microsoft's shobjidl_core.h documentation (vtable order)
    [ComImport, Guid("43826d1e-e718-42ee-bc55-a1e261c37bfe"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IShellItem
    {
        void BindToHandler(IntPtr pbc, ref Guid bhid, ref Guid riid, out IntPtr ppv);
        void GetParent(out IShellItem ppsi);
        void GetDisplayName(uint sigdnName, [MarshalAs(UnmanagedType.LPWStr)] out string ppszName);
        void GetAttributes(uint sfgaoMask, out uint psfgaoAttribs);
        void Compare(IShellItem psi, uint hint, out int piOrder);
    }

    [ComImport, Guid("d57c7288-d4ad-4768-be02-9d969532d960"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IFileOpenDialog
    {
        [PreserveSig] int Show(IntPtr parent);                         // IModalWindow
        void SetFileTypes(uint c, IntPtr filters);                     // IFileDialog
        void SetFileTypeIndex(uint i);
        void GetFileTypeIndex(out uint i);
        void Advise(IntPtr events, out uint cookie);
        void Unadvise(uint cookie);
        void SetOptions(uint fos);
        void GetOptions(out uint fos);
        void SetDefaultFolder(IShellItem psi);
        void SetFolder(IShellItem psi);
        void GetFolder(out IShellItem psi);
        void GetCurrentSelection(out IShellItem psi);
        void SetFileName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetFileName([MarshalAs(UnmanagedType.LPWStr)] out string name);
        void SetTitle([MarshalAs(UnmanagedType.LPWStr)] string title);
        void SetOkButtonLabel([MarshalAs(UnmanagedType.LPWStr)] string text);
        void SetFileNameLabel([MarshalAs(UnmanagedType.LPWStr)] string label);
        void GetResult(out IShellItem psi);
        void AddPlace(IShellItem psi, int fdap);
        void SetDefaultExtension([MarshalAs(UnmanagedType.LPWStr)] string ext);
        void Close(int hr);
        void SetClientGuid(ref Guid guid);
        void ClearClientData();
        void SetFilter(IntPtr filter);
        void GetResults(out IntPtr ppenum);                            // IFileOpenDialog
        void GetSelectedItems(out IntPtr ppsai);
    }

    [ComImport, Guid("DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7")]
    internal class FileOpenDialogCoClass { }

    internal static class Fos
    {
        public const uint PICKFOLDERS = 0x20, FORCEFILESYSTEM = 0x40, NOCHANGEDIR = 0x8, PATHMUSTEXIST = 0x800;
    }
}
