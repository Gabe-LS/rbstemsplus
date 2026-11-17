// The core's dialog model (core.h Dialogs, mac-core §3.7), read on the flow thread during
// host->dialog: its strings live until the callback returns, so they are copied here. One form
// covers the Mac Sheet's: title, message, labelled fields, rows of small buttons, then the buttons
// (stacked, the first the default; or inline, the first on the right). The core decides every
// text, button, default, Cancel and enabled state; the shell draws them (DialogWindow.cs) and
// answers by index, never by title.
using System;
using System.Collections.Generic;
using RbStemsPlus.Core;

namespace RbStemsPlus.Shell
{
    internal sealed class DialogRow
    {
        public string Label = "";
        public readonly List<string> Buttons = new List<string>();
        public readonly List<bool> Enabled = new List<bool>();
    }

    internal sealed class DialogForm
    {
        public int Id = -1;
        public string Title = "", Message = "";
        public readonly List<string> Buttons = new List<string>();
        public readonly List<bool> Enabled = new List<bool>(), Left = new List<bool>();
        public readonly List<string> FieldLabels = new List<string>(), FieldValues = new List<string>();
        public readonly List<DialogRow> Rows = new List<DialogRow>();
        public int Default, Cancel = -1;
        public bool Inline;

        public static DialogForm Read(IntPtr d)
        {
            string T(int what, int i = 0, int j = 0) => Core.Utf8(Core.core_dlg_text(d, what, i, j, out UIntPtr n), n);
            int I(int what, int i = 0, int j = 0) => Core.core_dlg_int(d, what, i, j);
            var f = new DialogForm
            {
                Id = I(CoreApi.CORE_DLG_ID),
                Title = T(CoreApi.CORE_DLG_TITLE),
                Message = T(CoreApi.CORE_DLG_MESSAGE),
                Default = I(CoreApi.CORE_DLG_DEFAULT),
                Cancel = I(CoreApi.CORE_DLG_CANCEL),
                Inline = I(CoreApi.CORE_DLG_INLINE) != 0,
            };
            for (int i = 0, n = Math.Max(0, I(CoreApi.CORE_DLG_N_BUTTONS)); i < n; i++)
            {
                f.Buttons.Add(T(CoreApi.CORE_DLG_BUTTON, i));
                f.Enabled.Add(I(CoreApi.CORE_DLG_BUTTON_ENABLED, i) != 0);
                f.Left.Add(I(CoreApi.CORE_DLG_BUTTON_LEFT, i) != 0);
            }
            for (int i = 0, n = Math.Max(0, I(CoreApi.CORE_DLG_N_FIELDS)); i < n; i++)
            {
                f.FieldLabels.Add(T(CoreApi.CORE_DLG_FIELD_LABEL, i));
                f.FieldValues.Add(T(CoreApi.CORE_DLG_FIELD_VALUE, i));
            }
            for (int i = 0, n = Math.Max(0, I(CoreApi.CORE_DLG_N_ROWS)); i < n; i++)
            {
                var row = new DialogRow { Label = T(CoreApi.CORE_DLG_ROW_LABEL, i) };
                for (int j = 0, m = Math.Max(0, I(CoreApi.CORE_DLG_N_ROW_BUTTONS, i)); j < m; j++)
                {
                    row.Buttons.Add(T(CoreApi.CORE_DLG_ROW_BUTTON, i, j));
                    row.Enabled.Add(I(CoreApi.CORE_DLG_ROW_BUTTON_ENABLED, i, j) != 0);
                }
                f.Rows.Add(row);
            }
            return f;
        }

        /// The order the buttons are drawn in, left to right: stacked, the core's order top to
        /// bottom; inline, the left-placed ones first, then the rest with the first on the right.
        public List<int> InlineOrder()
        {
            var order = new List<int>();
            for (int i = 0; i < Buttons.Count; i++) if (i < Left.Count && Left[i]) order.Add(i);
            for (int i = Buttons.Count - 1; i >= 0; i--) if (!(i < Left.Count && Left[i])) order.Add(i);
            return order;
        }
    }

    /// What the user did: a button (Button >= 0), a row's button (Row >= 0), or nothing (Cancel:
    /// the reply stays unset), with the fields' texts as they were left.
    internal sealed class DialogAnswer
    {
        public int Button = -1, Row = -1, RowButton = -1;
        public string[] Fields = new string[0];

        /// Into the core's reply (flow thread, before host->dialog returns).
        public void Fill(IntPtr r)
        {
            for (int i = 0; i < Fields.Length; i++) Core.core_reply_field(r, i, Fields[i] ?? "", CoreApi.Nts);
            if (Button >= 0) Core.core_reply_button(r, Button);
            else if (Row >= 0) Core.core_reply_row_button(r, Row, RowButton);
        }
    }
}
