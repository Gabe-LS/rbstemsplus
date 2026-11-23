// A menus.def row's Windows shortcut (key_win, mods_win; app.md §4.5) as WPF takes it: the name
// of a System.Windows.Input.Key, the modifiers, and the text the menu shows beside the title
// ("Ctrl+,", "F1", "Alt+F4"). ⌘ is Ctrl on Windows. And what the shell draws for each native
// Windows row (MenuNative). No WPF here (the Mac tests use it): the shell parses the key's name
// into a Key and maps each MenuNative to its action.
using System;
using RbStemsPlus.Core;

namespace RbStemsPlus.Shell
{
    internal sealed class MenuKey
    {
        public string Key;        // a System.Windows.Input.Key name: "OemComma", "D1", "R", "F1"
        public bool Ctrl, Shift, Alt;
        public string Text;       // the gesture's text in the menu

        /// null: no shortcut ("" key), or a key this table doesn't know (left out, never guessed).
        public static MenuKey Parse(string key, uint mods)
        {
            if (string.IsNullOrEmpty(key)) return null;
            string name, shown;
            char c = key[0];
            if (key.Length == 1 && c >= 'a' && c <= 'z') { name = char.ToUpperInvariant(c).ToString(); shown = name; }
            else if (key.Length == 1 && c >= '0' && c <= '9') { name = "D" + c; shown = key; }
            else if (key == ",") { name = "OemComma"; shown = key; }
            else if (key == ".") { name = "OemPeriod"; shown = key; }
            else if (key.Length >= 2 && key.Length <= 3 && c == 'F' && int.TryParse(key.Substring(1), out int f) && f >= 1 && f <= 24
                     && key.Substring(1) == f.ToString()) { name = key; shown = key; }
            else return null;
            var k = new MenuKey
            {
                Key = name,
                Ctrl = (mods & CoreApi.CORE_MOD_CMD_OR_CTRL) != 0,
                Shift = (mods & CoreApi.CORE_MOD_SHIFT) != 0,
                Alt = (mods & CoreApi.CORE_MOD_ALT) != 0,
            };
            k.Text = (k.Ctrl ? "Ctrl+" : "") + (k.Shift ? "Shift+" : "") + (k.Alt ? "Alt+" : "") + shown;
            return k;
        }
    }

    /// What the shell draws for each native Windows row (menus.def, kind native): Exit closes the
    /// window as its close box does (the core decides), Edit's items are WPF's commands, About is
    /// the shell's window. None: a native row this shell doesn't know.
    internal enum MenuNative { None, Exit, Cut, Copy, Paste, SelectAll, About }

    internal static class MenuNatives
    {
        public static MenuNative Of(int id)
        {
            switch ((Menu)id)
            {
                case Menu.M_WIN_APP_EXIT: return MenuNative.Exit;
                case Menu.M_WIN_EDIT_CUT: return MenuNative.Cut;
                case Menu.M_WIN_EDIT_COPY: return MenuNative.Copy;
                case Menu.M_WIN_EDIT_PASTE: return MenuNative.Paste;
                case Menu.M_WIN_EDIT_ALL: return MenuNative.SelectAll;
                case Menu.M_WIN_HELP_ABOUT: return MenuNative.About;
                default: return MenuNative.None;
            }
        }
    }
}
