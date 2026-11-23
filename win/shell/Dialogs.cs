// The windows the shell shows over the main window (app.md §4.4): the one dialog class for every
// core dialog (DialogForm: the welcome, Settings, every action's questions and alerts), the folder
// picker (IFileOpenDialog) and About. Each is owned by the main window, kept out of the taskbar
// and shown with ShowDialog, whose nested dispatcher frame keeps the main queue draining under it.
// Their texts are the core's; the layout is the shell's.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using RbStemsPlus.Core;
using CoreText = RbStemsPlus.Core.Text;

namespace RbStemsPlus.Shell
{
    internal sealed class DialogWindow : Window
    {
        private readonly DialogForm form;
        private readonly List<TextBox> fields = new List<TextBox>();
        private Button defaultButton;
        private DialogAnswer answer;

        public DialogWindow(DialogForm form, Window owner, Action<string> log)
        {
            new RenderRecovery(this, log);
            this.form = form;
            Owner = owner;
            Title = Texts.Get(CoreText.T_APP_NAME);
            ShowInTaskbar = false;
            ResizeMode = ResizeMode.NoResize;
            SizeToContent = SizeToContent.WidthAndHeight;
            WindowStartupLocation = owner != null ? WindowStartupLocation.CenterOwner : WindowStartupLocation.CenterScreen;
            UseLayoutRounding = true;
            Content = Build();
            // Narrator: the dialog by the core's title, its message as the help text (W9 checks the wording)
            System.Windows.Automation.AutomationProperties.SetName(this, form.Title);
            System.Windows.Automation.AutomationProperties.SetHelpText(this, form.Message);
            Loaded += (s, e) => FocusFirst();
        }

        // the first field, else the default button
        private void FocusFirst()
        {
            if (fields.Count > 0) { fields[0].Focus(); fields[0].SelectAll(); return; }
            if (defaultButton != null && defaultButton.IsEnabled) defaultButton.Focus();
        }

        /// Shown modally; the answer, or an empty one (Cancel: the reply stays unset) when the
        /// window was closed without a button.
        public DialogAnswer Run()
        {
            ShowDialog();
            var a = answer ?? new DialogAnswer();
            a.Fields = fields.ConvertAll(t => t.Text).ToArray();
            return a;
        }

        private UIElement Build()
        {
            var body = new StackPanel { Margin = new Thickness(20, 16, 20, 16), MaxWidth = 520, MinWidth = 340 };
            body.Children.Add(new TextBlock
            {
                Text = form.Title, FontWeight = FontWeights.SemiBold, FontSize = 14, TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 8),
            });
            if (form.Message.Length > 0)
                body.Children.Add(new TextBlock { Text = form.Message, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 0, 0, 12) });
            for (int i = 0; i < form.FieldLabels.Count; i++)
            {
                var label = new Label { Content = NoAccessKey(form.FieldLabels[i]), Padding = new Thickness(0, 2, 0, 2) };
                var box = new TextBox { Text = form.FieldValues[i], Margin = new Thickness(0, 0, 0, 8), MinWidth = 200 };
                label.Target = box;
                SetRawName(label, form.FieldLabels[i]);
                System.Windows.Automation.AutomationProperties.SetName(box, form.FieldLabels[i]);
                fields.Add(box);
                body.Children.Add(label);
                body.Children.Add(box);
            }
            for (int r = 0; r < form.Rows.Count; r++)
            {
                var row = form.Rows[r];
                var line = new DockPanel { Margin = new Thickness(0, 0, 0, 6), LastChildFill = true };
                var buttons = new StackPanel { Orientation = Orientation.Horizontal };
                DockPanel.SetDock(buttons, Dock.Right);
                for (int j = 0; j < row.Buttons.Count; j++)
                {
                    int ri = r, bj = j;
                    var b = new Button { Content = NoAccessKey(row.Buttons[j]), Padding = new Thickness(8, 1, 8, 1), Margin = new Thickness(6, 0, 0, 0), IsEnabled = row.Enabled[j] };
                    SetRawName(b, row.Buttons[j]);
                    b.Click += (s, e) => Answer(new DialogAnswer { Row = ri, RowButton = bj });
                    buttons.Children.Add(b);
                }
                line.Children.Add(buttons);
                line.Children.Add(new TextBlock { Text = row.Label, TextWrapping = TextWrapping.Wrap, VerticalAlignment = VerticalAlignment.Center });
                body.Children.Add(line);
            }
            body.Children.Add(form.Inline ? InlineButtons() : StackedButtons());
            return body;
        }

        // stacked: the core's order, top to bottom, each the column's width (Sheet.swift)
        private UIElement StackedButtons()
        {
            var p = new StackPanel { Margin = new Thickness(0, 4, 0, 0) };
            for (int i = 0; i < form.Buttons.Count; i++)
            {
                var b = MakeButton(i);
                b.Margin = new Thickness(0, 0, 0, 6);
                b.HorizontalAlignment = HorizontalAlignment.Stretch;
                p.Children.Add(b);
            }
            return p;
        }

        // inline: in a row, the first on the right; a left-placed one on the left
        private UIElement InlineButtons()
        {
            var line = new DockPanel { Margin = new Thickness(0, 8, 0, 0), LastChildFill = false };
            var right = new StackPanel { Orientation = Orientation.Horizontal };
            DockPanel.SetDock(right, Dock.Right);
            foreach (int i in form.InlineOrder())
            {
                var b = MakeButton(i);
                b.MinWidth = 80;
                b.Margin = new Thickness(8, 0, 0, 0);
                if (i < form.Left.Count && form.Left[i])
                {
                    b.Margin = new Thickness(0, 0, 8, 0);
                    DockPanel.SetDock(b, Dock.Left);
                    line.Children.Add(b);
                }
                else right.Children.Add(b);
            }
            line.Children.Add(right);
            return line;
        }

        private Button MakeButton(int i)
        {
            var b = new Button
            {
                Content = NoAccessKey(form.Buttons[i]), Padding = new Thickness(12, 3, 12, 3),
                IsDefault = i == form.Default, IsCancel = i == form.Cancel,
                IsEnabled = i >= form.Enabled.Count || form.Enabled[i],
            };
            SetRawName(b, form.Buttons[i]);
            if (i == form.Default) defaultButton = b;
            b.Click += (s, e) => Answer(new DialogAnswer { Button = i });
            return b;
        }

        /// A core text as a Label's or Button's content: '_' would mark an access key there, so it
        /// is doubled (as the menus do); automation names keep the raw text.
        internal static string NoAccessKey(string s) => s.Replace("_", "__");

        /// The raw text as the automation name of a Button or Label with string content: their
        /// peers strip one level of access-key markers from the name too (VM smoke: "First_One" read
        /// "FirstOne"), so the name is given escaped and reads raw.
        internal static void SetRawName(ContentControl c, string raw) =>
            System.Windows.Automation.AutomationProperties.SetName(c, NoAccessKey(raw));

        private void Answer(DialogAnswer a)
        {
            answer = a;
            Close();
        }
    }

    internal sealed class AboutWindow : Window
    {
        public AboutWindow(Window owner, string version, Action<string> log)
        {
            new RenderRecovery(this, log);
            Owner = owner;
            Title = Texts.Plain(CoreText.T_MENU_ABOUT);   // the menu title without its access key
            ShowInTaskbar = false;
            ResizeMode = ResizeMode.NoResize;
            SizeToContent = SizeToContent.WidthAndHeight;
            WindowStartupLocation = WindowStartupLocation.CenterOwner;
            var p = new StackPanel { Margin = new Thickness(28, 20, 28, 16), MinWidth = 280 };
            p.Children.Add(new TextBlock { Text = Texts.Get(CoreText.T_APP_NAME), FontSize = 16, FontWeight = FontWeights.SemiBold, HorizontalAlignment = HorizontalAlignment.Center });
            p.Children.Add(new TextBlock { Text = version, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 4, 0, 0) });
            p.Children.Add(new TextBlock { Text = Texts.Get(CoreText.T_ABOUT_CREDITS).Trim('\n'), TextAlignment = TextAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 12, 0, 0) });
            var ok = new Button { Content = DialogWindow.NoAccessKey(Texts.Get(CoreText.T_OK)), IsDefault = true, IsCancel = true, MinWidth = 80, Padding = new Thickness(12, 3, 12, 3), Margin = new Thickness(0, 16, 0, 0), HorizontalAlignment = HorizontalAlignment.Center };
            ok.Click += (s, e) => Close();
            p.Children.Add(ok);
            Content = p;
        }
    }

    internal static class FolderPicker
    {
        /// UI thread. IFileOpenDialog with FOS_PICKFOLDERS, its title and button the core's texts
        /// (T_PICK_FOLDER_MESSAGE, T_PICK_FOLDER_CHOOSE), starting at `start` when it exists.
        /// 0 chosen (path set), 1 cancelled, -1 not shown.
        public static int Show(Window owner, string start, out string path)
        {
            path = null;
            IFileOpenDialog dlg = null;
            try
            {
                dlg = (IFileOpenDialog)new FileOpenDialogCoClass();
                dlg.SetOptions(Fos.PICKFOLDERS | Fos.FORCEFILESYSTEM | Fos.NOCHANGEDIR | Fos.PATHMUSTEXIST);
                dlg.SetTitle(Texts.Get(CoreText.T_PICK_FOLDER_MESSAGE));
                dlg.SetOkButtonLabel(Texts.Get(CoreText.T_PICK_FOLDER_CHOOSE));
                if (!string.IsNullOrEmpty(start) && System.IO.Directory.Exists(start))
                {
                    try
                    {
                        Win32.SHCreateItemFromParsingName(start, IntPtr.Zero, typeof(IShellItem).GUID, out IShellItem folder);
                        dlg.SetFolder(folder);
                        Marshal.ReleaseComObject(folder);
                    }
                    catch (COMException) { }   // a start folder that can't be opened: the picker's own
                }
                int hr = dlg.Show(owner != null ? new WindowInteropHelper(owner).Handle : IntPtr.Zero);
                if (hr == Fos.ERROR_CANCELLED_HR) return 1;
                if (hr != 0) return -1;
                dlg.GetResult(out IShellItem item);
                item.GetDisplayName(Fos.SIGDN_FILESYSPATH, out path);
                Marshal.ReleaseComObject(item);
                return 0;
            }
            catch (COMException) { return -1; }
            finally { if (dlg != null) Marshal.ReleaseComObject(dlg); }
        }
    }
}
