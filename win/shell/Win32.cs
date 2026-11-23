// The few Windows calls the shell makes itself (OS components only): the DLL search order and
// the core's preload (app.md §3), the start-up refusal's TaskDialog (comctl32 v6, by the
// manifest), the logoff's shutdown block reason and the window messages it handles (§5.8, §5.9),
// and IFileOpenDialog for the folder picker (from Microsoft's shobjidl_core.h
// documentation, vtable order).
using System;
using System.Runtime.InteropServices;
using System.Security;

namespace RbStemsPlus.Shell
{
    [SuppressUnmanagedCodeSecurity]
    internal static class Win32
    {
        public const uint LOAD_LIBRARY_SEARCH_USER_DIRS = 0x400, LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x800;
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)] public static extern int SetDefaultDllDirectories(uint flags);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern IntPtr GetModuleHandleW(string name);
        // the old app's wait without the core (WU9): its pid and start time from update\pending
        public const uint SYNCHRONIZE = 0x00100000, PROCESS_QUERY_LIMITED_INFORMATION = 0x1000, WAIT_OBJECT_0 = 0;
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)] public static extern IntPtr OpenProcess(uint access, int inherit, uint pid);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)] public static extern int GetProcessTimes(IntPtr h, out long creation, out long exit, out long kernel, out long user);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)] public static extern uint WaitForSingleObject(IntPtr h, uint ms);
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)] public static extern int CloseHandle(IntPtr h);
        // the swap guard's kill (WU2): no WER dialog, unlike Environment.FailFast
        [DllImport("kernel32.dll", ExactSpelling = true)] public static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError = true, ExactSpelling = true)] public static extern int TerminateProcess(IntPtr process, uint code);

        [DllImport("comctl32.dll", ExactSpelling = true, CharSet = CharSet.Unicode, PreserveSig = true)]
        public static extern int TaskDialog(IntPtr owner, IntPtr inst, string title, string instruction, string content, int buttons, IntPtr icon, out int pressed);
        public const int TDCBF_OK_BUTTON = 1;
        public static readonly IntPtr TD_ERROR_ICON = new IntPtr(0xFFFE);

        // the logoff (app.md §5.9): the reason Windows shows while the session's end waits for us
        [DllImport("user32.dll", SetLastError = true, ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern int ShutdownBlockReasonCreate(IntPtr hwnd, string reason);
        [DllImport("user32.dll", SetLastError = true, ExactSpelling = true)] public static extern int ShutdownBlockReasonDestroy(IntPtr hwnd);
        public const int WM_ENDSESSION = 0x0016, WM_DEVICECHANGE = 0x0219;
        // WM_DEVICECHANGE's wParam (dbt.h) and DEV_BROADCAST_HDR's dbch_devicetype offset; a
        // DEV_BROADCAST_VOLUME's dbcv_unitmask follows the 12-byte header
        public const int DBT_DEVICEARRIVAL = 0x8000, DBT_DEVICEREMOVECOMPLETE = 0x8004, DBT_DEVTYP_VOLUME = 2;

        [DllImport("shell32.dll", ExactSpelling = true, CharSet = CharSet.Unicode, PreserveSig = false)]
        public static extern void SHCreateItemFromParsingName(string path, IntPtr pbc, [MarshalAs(UnmanagedType.LPStruct)] Guid riid, out IShellItem item);

        // the activation aggregator's source (app.md §4.3)
        public const int WM_ACTIVATEAPP = 0x001C;

        // the window's size and position (app.md §4.6)
        public const int SW_SHOWNORMAL = 1, SW_SHOWMINIMIZED = 2, SW_SHOWMAXIMIZED = 3;
        public const uint MONITOR_DEFAULTTONULL = 0, MONITOR_DEFAULTTOPRIMARY = 1;
        public const int MDT_EFFECTIVE_DPI = 0;
        [DllImport("user32.dll", SetLastError = true, ExactSpelling = true)] public static extern int GetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT wp);
        [DllImport("user32.dll", SetLastError = true, ExactSpelling = true)] public static extern int SetWindowPlacement(IntPtr hwnd, ref WINDOWPLACEMENT wp);
        [DllImport("user32.dll", ExactSpelling = true)] public static extern IntPtr MonitorFromRect(ref RECT rc, uint flags);
        [DllImport("user32.dll", ExactSpelling = true)] public static extern IntPtr MonitorFromPoint(POINT pt, uint flags);
        [DllImport("user32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern int GetMonitorInfoW(IntPtr monitor, ref MONITORINFO mi);
        [DllImport("shcore.dll", ExactSpelling = true)] public static extern int GetDpiForMonitor(IntPtr monitor, int type, out uint dpiX, out uint dpiY);

        // repaint and render recovery
        [DllImport("user32.dll", ExactSpelling = true)] public static extern int InvalidateRect(IntPtr hwnd, IntPtr rect, int erase);
        [DllImport("user32.dll", ExactSpelling = true, CharSet = CharSet.Unicode)] public static extern int RegisterWindowMessageW(string name);
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct POINT { public int x, y; }

    [StructLayout(LayoutKind.Sequential)]
    internal struct MONITORINFO
    {
        public int cbSize;
        public RECT rcMonitor, rcWork;
        public uint dwFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct WINDOWPLACEMENT
    {
        public int length, flags, showCmd;
        public POINT minPosition, maxPosition;
        public RECT normalPosition;
    }

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
        public const uint SIGDN_FILESYSPATH = 0x80058000;
        public const int ERROR_CANCELLED_HR = unchecked((int)0x800704C7);
    }
}
