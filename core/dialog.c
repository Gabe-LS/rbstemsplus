/* Dialogs and their answers (mac-core §3.7, the ruling's D5): accessors over the core's dialog,
 * the reply setters, and Settings' enable rules. */
#include "internal.h"

static int bit_clear(uint32_t mask, int32_t i) { return i >= 0 && i < 32 && !(mask >> i & 1u); }

const char *core_dlg_text(const core_dialog *d, core_dlg_what what, int32_t i, int32_t j, size_t *len) {
    const char *s = NULL;
    if (d) switch (what) {
    case CORE_DLG_TITLE: s = d->title; break;
    case CORE_DLG_MESSAGE: s = d->message; break;
    case CORE_DLG_BUTTON: if (i >= 0 && i < d->nbuttons) s = d->buttons[i]; break;
    case CORE_DLG_FIELD_LABEL: if (i >= 0 && i < d->nfields) s = d->field_label[i]; break;
    case CORE_DLG_FIELD_VALUE: if (i >= 0 && i < d->nfields) s = d->field_value[i]; break;
    case CORE_DLG_ROW_LABEL: if (i >= 0 && i < d->nrows) s = d->row_label[i]; break;
    case CORE_DLG_ROW_BUTTON: if (i >= 0 && i < d->nrows && j >= 0 && j < d->row_nbuttons[i]) s = d->row_buttons[i][j]; break;
    default: break;
    }
    if (!s) s = "";
    if (len) *len = strlen(s);
    return s;
}

int32_t core_dlg_int(const core_dialog *d, core_dlg_what what, int32_t i, int32_t j) {
    if (!d) return 0;
    switch (what) {
    case CORE_DLG_N_BUTTONS: return d->nbuttons;
    case CORE_DLG_N_FIELDS: return d->nfields;
    case CORE_DLG_N_ROWS: return d->nrows;
    case CORE_DLG_N_ROW_BUTTONS: return i >= 0 && i < d->nrows ? d->row_nbuttons[i] : 0;
    case CORE_DLG_DEFAULT: return d->def;
    case CORE_DLG_CANCEL: return d->cancel;
    case CORE_DLG_INLINE: return d->inline_;
    case CORE_DLG_ID: return d->id;
    case CORE_DLG_BUTTON_ENABLED: return i < d->nbuttons && bit_clear(d->disabled_buttons, i);
    case CORE_DLG_ROW_BUTTON_ENABLED:
        return i >= 0 && i < d->nrows && j < d->row_nbuttons[i] && bit_clear(d->disabled_row_buttons[i], j);
    case CORE_DLG_BUTTON_LEFT: return i >= 0 && i < d->nbuttons && i < 32 && (d->left_buttons >> i & 1u);
    default: return 0;
    }
}

void reply_init(core_reply *r, arena *a) {
    memset(r, 0, sizeof *r);
    r->button = r->row = r->row_button = -1;
    r->a = a;
}

void core_reply_button(core_reply *r, int32_t i) {
    if (!r) return;
    r->button = i;
    r->row = r->row_button = -1;
}

void core_reply_row_button(core_reply *r, int32_t row, int32_t j) {
    if (!r) return;
    r->button = -1;
    r->row = row;
    r->row_button = j;
}

void core_reply_field(core_reply *r, int32_t i, const char *v, size_t len) {
    if (!r || i < 0 || i >= DLG_MAX_FIELDS || (!v && len)) return;
    if (len == CORE_NTS) len = strlen(v);
    const char *p = arena_strdup(r->a, v ? v : "", v ? len : 0);
    if (!p) return;
    r->fields[i] = p;
    r->field_lens[i] = len;
}

/* Settings.swift:29-32, with the review's corrected reading: Save needs a readable config.ini;
 * Clear also needs a target to clear (this Mac's folder exists, or a drive target), not a nonzero
 * size; Change… also needs a location that can be chosen (Stems Cache not installed, or a bridge
 * that supports locations); Use This Mac also needs a location that isn't This Mac already. */
void dlg_settings_enable(core_dialog *d, int readable, int clear_target, int can_choose, int this_mac) {
    d->disabled_buttons = 0;
    d->disabled_row_buttons[0] = 0;
    if (!readable) d->disabled_buttons |= 1u << 0;                       /* Save */
    if (!(readable && clear_target)) d->disabled_buttons |= 1u << 1;     /* Clear Saved Stems */
    if (!(readable && can_choose)) d->disabled_row_buttons[0] |= 1u << 0; /* Change… */
    if (!(readable && !this_mac)) d->disabled_row_buttons[0] |= 1u << 1; /* Use This Mac */
}
