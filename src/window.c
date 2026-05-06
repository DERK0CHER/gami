#include "window.h"

#include <math.h>
#include <cairo-svg.h>

#include "canvas.h"
#include "canvas3d.h"
#include "crease.h"
#include "crease_canvas.h"
#include "crease_canvas3d.h"

/* Must match canvas.c. The canvas exposes mm-to-virtual conversion so
 * we don't have to know the virtual transform here, but we do need to
 * know the paper's mm extent to bound the spin buttons. */
#define PAPER_W_MM 560.0
#define PAPER_H_MM 560.0

struct _OrigamiWindow {
    AdwApplicationWindow parent_instance;

    OrigamiCanvas        *canvas;
    OrigamiCanvas3D      *canvas3d;
    GtkLabel             *status_label;
    AdwToastOverlay      *toast_overlay;
    GtkButton            *undo_button;

    /* Header-bar widgets we need to keep in sync with canvas state. */
    GtkToggleButton      *tool_select_btn;
    GtkToggleButton      *tool_fold_btn;
    GtkToggleButton      *tool_pull_btn;
    GtkToggleButton      *ruler_btn;
    GtkToggleButton      *sidebar_btn;

    /* Right sidebar listing fold steps. */
    AdwOverlaySplitView  *split;
    GtkListBox           *steps_list;
    GtkListBox           *layers_list;
    GtkLabel             *target_label;

    /* Coordinate-input form. */
    GtkSpinButton        *x1_spin;
    GtkSpinButton        *y1_spin;
    GtkSpinButton        *x2_spin;
    GtkSpinButton        *y2_spin;

    /* Guard so we don't recursively re-select while rebuilding. */
    gboolean              suppress_layer_select;

    /* View switcher between 2D editor and 3D preview. */
    AdwViewStack         *view_stack;

    /* New crease-graph editor — shares the view stack. */
    OrigamiCreaseCanvas   *crease_canvas;
    OrigamiCreaseCanvas3D *crease_canvas3d;
    GtkScale              *completion_scale;
    GtkToggleButton       *m_btn;
    GtkToggleButton       *v_btn;
};

G_DEFINE_FINAL_TYPE (OrigamiWindow, origami_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---------- ui state sync ---------- */

static void
update_status (OrigamiWindow *self)
{
    OrigamiCanvasState s = origami_canvas_get_state (self->canvas);
    OrigamiTool t = origami_canvas_get_tool (self->canvas);
    guint sel = origami_canvas_selection_size (self->canvas);

    char buf[256];
    if (t == ORIGAMI_TOOL_PULL) {
        g_snprintf (buf, sizeof buf,
                    "Pull tool — drag a surface to slide it; Shift-drag rotates.");
    } else if (t == ORIGAMI_TOOL_SELECT) {
        if (sel == 0)
            g_snprintf (buf, sizeof buf,
                "Select tool — click a surface to select it. "
                "Ctrl/Shift-click adds. Switch to Fold to draw a line.");
        else
            g_snprintf (buf, sizeof buf,
                "Select tool — %u surface%s selected. "
                "Ctrl/Shift-click to add or remove. "
                "Switch to Fold to draw a line.",
                sel, sel == 1 ? "" : "s");
    } else {
        const char *target = (sel > 0)
            ? "selected surfaces"
            : "every surface that crosses the line";
        const char *step;
        switch (s) {
        case ORIGAMI_CANVAS_STATE_FIRST_POINT:
            step = "Click the first point of a fold line";
            break;
        case ORIGAMI_CANVAS_STATE_SECOND_POINT:
            step = "Click the second point (Shift = 15° snap, Ctrl = vertex snap)";
            break;
        case ORIGAMI_CANVAS_STATE_PICK_SIDE:
            step = "Click the side to fold over (Esc to cancel)";
            break;
        default:
            step = "";
        }
        g_snprintf (buf, sizeof buf, "%s — folds %s.", step, target);
    }
    gtk_label_set_text (self->status_label, buf);

    gtk_widget_set_sensitive (
        GTK_WIDGET (self->undo_button),
        origami_canvas_can_undo (self->canvas));
}

static void
rebuild_steps (OrigamiWindow *self)
{
    GtkListBox *box = self->steps_list;
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child (GTK_WIDGET (box))) != NULL)
        gtk_list_box_remove (box, child);

    OrigamiPaper *p = origami_canvas_get_paper (self->canvas);
    if (!p) return;

    for (guint i = 0; i < p->records->len; i++) {
        OrigamiFoldRecord *r = &g_array_index (p->records,
                                               OrigamiFoldRecord, i);
        char title[64];
        g_snprintf (title, sizeof title, "Step %u", i + 1);

        GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_top    (row_box, 6);
        gtk_widget_set_margin_bottom (row_box, 6);
        gtk_widget_set_margin_start  (row_box, 12);
        gtk_widget_set_margin_end    (row_box, 12);

        GtkWidget *t = gtk_label_new (title);
        gtk_widget_add_css_class (t, "heading");
        gtk_label_set_xalign (GTK_LABEL (t), 0.0);
        gtk_box_append (GTK_BOX (row_box), t);

        GtkWidget *d = gtk_label_new (r->summary ? r->summary : "");
        gtk_label_set_wrap (GTK_LABEL (d), TRUE);
        gtk_label_set_xalign (GTK_LABEL (d), 0.0);
        gtk_widget_add_css_class (d, "dim-label");
        gtk_widget_add_css_class (d, "caption");
        gtk_box_append (GTK_BOX (row_box), d);

        GtkWidget *row = gtk_list_box_row_new ();
        gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), row_box);
        g_object_set_data (G_OBJECT (row), "step-index",
                           GUINT_TO_POINTER (i + 1));
        gtk_list_box_append (box, row);
    }

    if (p->records->len == 0) {
        GtkWidget *empty = gtk_label_new ("No folds yet.\nDraw a line on the paper to begin.");
        gtk_widget_add_css_class (empty, "dim-label");
        gtk_label_set_justify (GTK_LABEL (empty), GTK_JUSTIFY_CENTER);
        gtk_widget_set_margin_top    (empty, 24);
        gtk_widget_set_margin_bottom (empty, 24);
        gtk_widget_set_margin_start  (empty, 12);
        gtk_widget_set_margin_end    (empty, 12);
        GtkWidget *empty_row = g_object_new (GTK_TYPE_LIST_BOX_ROW,
                                             "child", empty,
                                             "selectable", FALSE,
                                             "activatable", FALSE,
                                             NULL);
        gtk_list_box_append (box, empty_row);
    }
}

static void
on_step_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    gpointer p = g_object_get_data (G_OBJECT (row), "step-index");
    if (!p) return;
    guint n = GPOINTER_TO_UINT (p);
    origami_canvas_replay_to (self->canvas, n);
}

static void
rebuild_layers (OrigamiWindow *self)
{
    if (!self->layers_list) return;
    GtkListBox *box = self->layers_list;
    GtkWidget *child;

    self->suppress_layer_select = TRUE;
    while ((child = gtk_widget_get_first_child (GTK_WIDGET (box))) != NULL)
        gtk_list_box_remove (box, child);

    OrigamiPaper *p = origami_canvas_get_paper (self->canvas);
    guint sel_count = origami_canvas_selection_size (self->canvas);

    if (p && p->current) {
        for (guint i = 0; i < p->current->layers->len; i++) {
            OrigamiLayer *l = g_ptr_array_index (p->current->layers, i);

            GtkWidget *row_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top    (row_box, 4);
            gtk_widget_set_margin_bottom (row_box, 4);
            gtk_widget_set_margin_start  (row_box, 12);
            gtk_widget_set_margin_end    (row_box, 12);

            char buf[64];
            g_snprintf (buf, sizeof buf, "Surface #%u", l->id);
            GtkWidget *name = gtk_label_new (buf);
            gtk_label_set_xalign (GTK_LABEL (name), 0.0);
            gtk_widget_set_hexpand (name, TRUE);
            gtk_box_append (GTK_BOX (row_box), name);

            const char *side = l->flipped ? "back" : "front";
            GtkWidget *side_lbl = gtk_label_new (side);
            gtk_widget_add_css_class (side_lbl, "dim-label");
            gtk_widget_add_css_class (side_lbl, "caption");
            gtk_box_append (GTK_BOX (row_box), side_lbl);

            GtkWidget *row = gtk_list_box_row_new ();
            gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (row), row_box);
            g_object_set_data (G_OBJECT (row), "layer-id",
                               GUINT_TO_POINTER (l->id));
            gtk_list_box_append (box, row);
            if (origami_canvas_is_selected (self->canvas, l->id))
                gtk_list_box_select_row (box, GTK_LIST_BOX_ROW (row));
        }
    }

    if (self->target_label) {
        char tbuf[80];
        if (sel_count == 0)
            g_snprintf (tbuf, sizeof tbuf,
                        "No selection — folds apply to all surfaces");
        else
            g_snprintf (tbuf, sizeof tbuf,
                        "%u surface%s selected — folds apply to those",
                        sel_count, sel_count == 1 ? "" : "s");
        gtk_label_set_text (self->target_label, tbuf);
    }

    self->suppress_layer_select = FALSE;
}

static void
on_layer_row_selected (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    /* Multi-select mode emits "selected-rows-changed" instead — we use
     * that path for sync.  This stub stays connected so single-select
     * fallbacks behave. */
}

static void
on_layer_rows_changed (GtkListBox *box, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    if (self->suppress_layer_select) return;

    /* Reflect the list box's selection into the canvas selection set. */
    origami_canvas_clear_selection (self->canvas);
    GList *rows = gtk_list_box_get_selected_rows (box);
    for (GList *node = rows; node; node = node->next) {
        GtkListBoxRow *row = node->data;
        guint id = GPOINTER_TO_UINT (
            g_object_get_data (G_OBJECT (row), "layer-id"));
        if (id != 0)
            origami_canvas_add_to_selection (self->canvas, id);
    }
    g_list_free (rows);
    rebuild_layers (self);
}

static void
on_clear_target (GtkButton *btn, gpointer ud)
{
    OrigamiWindow *self = ud;
    origami_canvas_clear_selection (self->canvas);
    rebuild_layers (self);
}

static void
on_fold_coords_left (GtkButton *btn, gpointer ud)
{
    OrigamiWindow *self = ud;
    origami_canvas_fold_mm (self->canvas,
        gtk_spin_button_get_value (self->x1_spin),
        gtk_spin_button_get_value (self->y1_spin),
        gtk_spin_button_get_value (self->x2_spin),
        gtk_spin_button_get_value (self->y2_spin),
        -1);
}

static void
on_fold_coords_right (GtkButton *btn, gpointer ud)
{
    OrigamiWindow *self = ud;
    origami_canvas_fold_mm (self->canvas,
        gtk_spin_button_get_value (self->x1_spin),
        gtk_spin_button_get_value (self->y1_spin),
        gtk_spin_button_get_value (self->x2_spin),
        gtk_spin_button_get_value (self->y2_spin),
        +1);
}

static void
on_canvas_state_changed (OrigamiCanvas *canvas, OrigamiWindow *self)
{
    update_status (self);
    rebuild_layers (self);
    origami_canvas3d_redraw (self->canvas3d);
}

static void
on_history_changed (OrigamiCanvas *canvas, OrigamiWindow *self)
{
    rebuild_steps (self);
    rebuild_layers (self);
    origami_canvas3d_redraw (self->canvas3d);
}

/* ---------- export ---------- */

static void
render_full (OrigamiWindow *self, cairo_t *cr, int w, int h)
{
    /* Re-use the canvas' draw_func by giving it a temporary surface.
     * GtkDrawingArea exposes its draw function callback only via the
     * widget vfunc, so we go via gtk_widget_snapshot_child... but for a
     * single drawing area that's awkward. Instead we just invoke our
     * own simplified path: ask the widget to draw to the cr by using
     * gtk_widget_size_allocate + gtk_widget_snapshot. The simpler trick
     * is to pull the polygons out of the paper directly. */
    OrigamiPaper *p = origami_canvas_get_paper (self->canvas);
    if (!p || !p->current) return;

    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);

    /* The canvas uses a 1000x1000 virtual field. Fit it into w x h. */
    double s = MIN (w, h) / 1000.0;
    double tx = (w - 1000 * s) / 2.0;
    double ty = (h - 1000 * s) / 2.0;
    cairo_translate (cr, tx, ty);
    cairo_scale (cr, s, s);

    /* shadow */
    cairo_save (cr);
    cairo_translate (cr, 4, 6);
    cairo_set_source_rgba (cr, 0, 0, 0, 0.10);
    for (guint i = 0; i < p->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (p->current->layers, i);
        GArray *poly = origami_layer_world_polygon (l);
        if (poly->len >= 3) {
            OrigamiPoint *v = (OrigamiPoint *) poly->data;
            cairo_move_to (cr, v[0].x, v[0].y);
            for (guint j = 1; j < poly->len; j++)
                cairo_line_to (cr, v[j].x, v[j].y);
            cairo_close_path (cr);
            cairo_fill (cr);
        }
        g_array_free (poly, TRUE);
    }
    cairo_restore (cr);

    for (guint i = 0; i < p->current->layers->len; i++) {
        OrigamiLayer *l = g_ptr_array_index (p->current->layers, i);
        GArray *poly = origami_layer_world_polygon (l);
        if (poly->len < 3) { g_array_free (poly, TRUE); continue; }
        OrigamiPoint *v = (OrigamiPoint *) poly->data;
        cairo_move_to (cr, v[0].x, v[0].y);
        for (guint j = 1; j < poly->len; j++)
            cairo_line_to (cr, v[j].x, v[j].y);
        cairo_close_path (cr);
        if (l->flipped) cairo_set_source_rgb (cr, 0.91, 0.78, 0.55);
        else            cairo_set_source_rgb (cr, 0.992, 0.969, 0.929);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, 0, 0, 0, 0.18);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);
        g_array_free (poly, TRUE);
    }
}

static void
toast (OrigamiWindow *self, const char *text)
{
    AdwToast *t = adw_toast_new (text);
    adw_toast_set_timeout (t, 3);
    adw_toast_overlay_add_toast (self->toast_overlay, t);
}

static void
do_export_png (OrigamiWindow *self, const char *path)
{
    int w = 1200, h = 1200;
    cairo_surface_t *surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create (surf);
    render_full (self, cr, w, h);
    cairo_destroy (cr);
    cairo_status_t st = cairo_surface_write_to_png (surf, path);
    cairo_surface_destroy (surf);
    char msg[256];
    if (st == CAIRO_STATUS_SUCCESS)
        g_snprintf (msg, sizeof msg, "Exported PNG to %s", path);
    else
        g_snprintf (msg, sizeof msg, "PNG export failed: %s",
                    cairo_status_to_string (st));
    toast (self, msg);
}

static void
do_export_svg (OrigamiWindow *self, const char *path)
{
    int w = 1200, h = 1200;
    cairo_surface_t *surf = cairo_svg_surface_create (path, w, h);
    cairo_t *cr = cairo_create (surf);
    render_full (self, cr, w, h);
    cairo_destroy (cr);
    cairo_status_t st = cairo_surface_status (surf);
    cairo_surface_destroy (surf);
    char msg[256];
    if (st == CAIRO_STATUS_SUCCESS)
        g_snprintf (msg, sizeof msg, "Exported SVG to %s", path);
    else
        g_snprintf (msg, sizeof msg, "SVG export failed: %s",
                    cairo_status_to_string (st));
    toast (self, msg);
}

static void
do_export_instructions (OrigamiWindow *self, const char *path)
{
    OrigamiPaper *p = origami_canvas_get_paper (self->canvas);
    GString *s = g_string_new ("# Origami Fold — Instructions\n\n");
    if (!p || p->records->len == 0) {
        g_string_append (s, "_No folds recorded yet._\n");
    } else {
        for (guint i = 0; i < p->records->len; i++) {
            OrigamiFoldRecord *r = &g_array_index (p->records,
                                                   OrigamiFoldRecord, i);
            g_string_append_printf (s, "%u. %s\n", i + 1,
                                    r->summary ? r->summary : "(fold)");
        }
    }
    GError *err = NULL;
    gboolean ok = g_file_set_contents (path, s->str, -1, &err);
    char msg[256];
    if (ok) g_snprintf (msg, sizeof msg, "Exported instructions to %s", path);
    else    g_snprintf (msg, sizeof msg, "Export failed: %s",
                        err ? err->message : "unknown");
    g_clear_error (&err);
    g_string_free (s, TRUE);
    toast (self, msg);
}

typedef enum {
    EXPORT_PNG, EXPORT_SVG, EXPORT_MD,
} ExportKind;

static void
on_save_response (GObject *src, GAsyncResult *res, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    ExportKind kind = GPOINTER_TO_INT (
        g_object_get_data (G_OBJECT (src), "export-kind"));
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish (GTK_FILE_DIALOG (src),
                                               res, &err);
    if (!file) {
        g_clear_error (&err);
        return;
    }
    char *path = g_file_get_path (file);
    if (path) {
        switch (kind) {
        case EXPORT_PNG: do_export_png          (self, path); break;
        case EXPORT_SVG: do_export_svg          (self, path); break;
        case EXPORT_MD:  do_export_instructions (self, path); break;
        }
        g_free (path);
    }
    g_object_unref (file);
}

static void
present_save_dialog (OrigamiWindow *self,
                     ExportKind kind,
                     const char *title,
                     const char *suggested)
{
    GtkFileDialog *dlg = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dlg, title);
    gtk_file_dialog_set_initial_name (dlg, suggested);
    g_object_set_data (G_OBJECT (dlg), "export-kind",
                       GINT_TO_POINTER (kind));
    gtk_file_dialog_save (dlg, GTK_WINDOW (self), NULL,
                          on_save_response, self);
    g_object_unref (dlg);
}

/* ---------- actions ---------- */

static void
action_new (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    origami_canvas_reset (self->canvas);
    AdwToast *t = adw_toast_new ("New paper");
    adw_toast_set_timeout (t, 1);
    adw_toast_overlay_add_toast (self->toast_overlay, t);
}

static void
action_undo   (GSimpleAction *a, GVariant *p, gpointer u)
{ origami_canvas_undo   (((OrigamiWindow *) u)->canvas); }

static void
action_cancel (GSimpleAction *a, GVariant *p, gpointer u)
{ origami_canvas_cancel (((OrigamiWindow *) u)->canvas); }

static void
action_about (GSimpleAction *a, GVariant *p, gpointer u)
{
    OrigamiWindow *self = u;
    const char *developers[] = { "Origami Fold contributors", NULL };

    AdwDialog *about = g_object_new (
        ADW_TYPE_ABOUT_DIALOG,
        "application-name",  "Origami Fold",
        "application-icon",  "applications-graphics-symbolic",
        "version",           "0.2.0",
        "developers",        developers,
        "license-type",      GTK_LICENSE_GPL_3_0,
        "comments",
        "Fold paper page-by-page on a measured workspace.\n\n"
        "Shortcuts:\n"
        "  Ctrl+N — new sheet\n"
        "  Ctrl+Z — undo\n"
        "  Esc    — cancel current fold\n"
        "  Shift  — snap fold-line angle to 15°\n"
        "  Ctrl   — snap endpoint to nearest vertex\n"
        "  F1     — about",
        NULL);
    adw_dialog_present (about, GTK_WIDGET (self));
}

static void
action_export_png (GSimpleAction *a, GVariant *p, gpointer u)
{ present_save_dialog (u, EXPORT_PNG, "Export as PNG", "origami.png"); }

static void
action_export_svg (GSimpleAction *a, GVariant *p, gpointer u)
{ present_save_dialog (u, EXPORT_SVG, "Export as SVG", "origami.svg"); }

static void
action_export_md (GSimpleAction *a, GVariant *p, gpointer u)
{ present_save_dialog (u, EXPORT_MD, "Export instructions", "origami.md"); }

static void
action_toggle_sidebar (GSimpleAction *a, GVariant *p, gpointer u)
{
    OrigamiWindow *self = u;
    gboolean cur = adw_overlay_split_view_get_show_sidebar (self->split);
    adw_overlay_split_view_set_show_sidebar (self->split, !cur);
}

static const GActionEntry win_actions[] = {
    { "new",            action_new,            NULL, NULL, NULL },
    { "undo",           action_undo,           NULL, NULL, NULL },
    { "cancel",         action_cancel,         NULL, NULL, NULL },
    { "about",          action_about,          NULL, NULL, NULL },
    { "export-png",     action_export_png,     NULL, NULL, NULL },
    { "export-svg",     action_export_svg,     NULL, NULL, NULL },
    { "export-md",      action_export_md,      NULL, NULL, NULL },
    { "toggle-sidebar", action_toggle_sidebar, NULL, NULL, NULL },
};

/* ---------- header-bar toggles ---------- */

static void
on_tool_select_toggled (GtkToggleButton *btn, OrigamiWindow *self)
{
    if (gtk_toggle_button_get_active (btn))
        origami_canvas_set_tool (self->canvas, ORIGAMI_TOOL_SELECT);
}

static void
on_tool_fold_toggled (GtkToggleButton *btn, OrigamiWindow *self)
{
    if (gtk_toggle_button_get_active (btn))
        origami_canvas_set_tool (self->canvas, ORIGAMI_TOOL_FOLD);
}

static void
on_tool_pull_toggled (GtkToggleButton *btn, OrigamiWindow *self)
{
    if (gtk_toggle_button_get_active (btn))
        origami_canvas_set_tool (self->canvas, ORIGAMI_TOOL_PULL);
}

static void
on_ruler_toggled (GtkToggleButton *btn, OrigamiWindow *self)
{
    origami_canvas_set_show_rulers (self->canvas,
                                    gtk_toggle_button_get_active (btn));
}

static void
on_sidebar_toggled (GtkToggleButton *btn, OrigamiWindow *self)
{
    adw_overlay_split_view_set_show_sidebar (self->split,
        gtk_toggle_button_get_active (btn));
}

static void
on_split_show_sidebar (GObject *obj, GParamSpec *ps, gpointer ud)
{
    OrigamiWindow *self = ud;
    gboolean shown = adw_overlay_split_view_get_show_sidebar (self->split);
    if (gtk_toggle_button_get_active (self->sidebar_btn) != shown)
        gtk_toggle_button_set_active (self->sidebar_btn, shown);
}

/* ---------- construction ---------- */

static GtkWidget *
build_header (OrigamiWindow *self)
{
    GtkWidget *header = adw_header_bar_new ();

    AdwViewSwitcher *switcher = ADW_VIEW_SWITCHER (adw_view_switcher_new ());
    adw_view_switcher_set_policy (switcher, ADW_VIEW_SWITCHER_POLICY_WIDE);
    /* Stack assigned later, after view_stack exists. */
    adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                     GTK_WIDGET (switcher));
    g_object_set_data (G_OBJECT (header), "switcher", switcher);

    GtkWidget *new_btn = gtk_button_new_from_icon_name ("document-new-symbolic");
    gtk_widget_set_tooltip_text (new_btn, "New paper");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (new_btn), "win.new");
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), new_btn);

    GtkWidget *undo_btn = gtk_button_new_from_icon_name ("edit-undo-symbolic");
    gtk_widget_set_tooltip_text (undo_btn, "Undo");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (undo_btn), "win.undo");
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), undo_btn);
    self->undo_button = GTK_BUTTON (undo_btn);

    /* Tool group: Select / Fold / Pull. */
    GtkWidget *tool_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (tool_box, "linked");

    GtkWidget *select_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (select_btn), "edit-select-all-symbolic");
    gtk_widget_set_tooltip_text (select_btn,
        "Select tool — click a surface to select it; "
        "Ctrl/Shift-click to add or toggle");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (select_btn), TRUE);
    g_signal_connect (select_btn, "toggled",
                      G_CALLBACK (on_tool_select_toggled), self);
    gtk_box_append (GTK_BOX (tool_box), select_btn);
    self->tool_select_btn = GTK_TOGGLE_BUTTON (select_btn);

    GtkWidget *fold_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (fold_btn), "edit-cut-symbolic");
    gtk_widget_set_tooltip_text (fold_btn,
        "Fold tool — draw a line, click the side to fold over");
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (fold_btn),
                                 GTK_TOGGLE_BUTTON (select_btn));
    g_signal_connect (fold_btn, "toggled",
                      G_CALLBACK (on_tool_fold_toggled), self);
    gtk_box_append (GTK_BOX (tool_box), fold_btn);
    self->tool_fold_btn = GTK_TOGGLE_BUTTON (fold_btn);

    GtkWidget *pull_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (pull_btn), "object-flip-horizontal-symbolic");
    gtk_widget_set_tooltip_text (pull_btn, "Pull tool — drag a surface");
    gtk_toggle_button_set_group (GTK_TOGGLE_BUTTON (pull_btn),
                                 GTK_TOGGLE_BUTTON (select_btn));
    g_signal_connect (pull_btn, "toggled",
                      G_CALLBACK (on_tool_pull_toggled), self);
    gtk_box_append (GTK_BOX (tool_box), pull_btn);
    self->tool_pull_btn = GTK_TOGGLE_BUTTON (pull_btn);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), tool_box);

    GtkWidget *ruler_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (ruler_btn), "view-grid-symbolic");
    gtk_widget_set_tooltip_text (ruler_btn, "Show rulers");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (ruler_btn), TRUE);
    g_signal_connect (ruler_btn, "toggled",
                      G_CALLBACK (on_ruler_toggled), self);
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), ruler_btn);
    self->ruler_btn = GTK_TOGGLE_BUTTON (ruler_btn);

    /* Sidebar toggle. */
    GtkWidget *sidebar_btn = gtk_toggle_button_new ();
    gtk_button_set_icon_name (GTK_BUTTON (sidebar_btn), "view-list-symbolic");
    gtk_widget_set_tooltip_text (sidebar_btn, "Show fold instructions");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (sidebar_btn), TRUE);
    g_signal_connect (sidebar_btn, "toggled",
                      G_CALLBACK (on_sidebar_toggled), self);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), sidebar_btn);
    self->sidebar_btn = GTK_TOGGLE_BUTTON (sidebar_btn);

    /* Main menu. */
    GMenu *menu = g_menu_new ();
    g_menu_append (menu, "_New Paper",        "win.new");
    g_menu_append (menu, "_Undo",             "win.undo");
    GMenu *exp = g_menu_new ();
    g_menu_append (exp, "Export _PNG…",       "win.export-png");
    g_menu_append (exp, "Export _SVG…",       "win.export-svg");
    g_menu_append (exp, "Export _Instructions…", "win.export-md");
    g_menu_append_section (menu, "Export", G_MENU_MODEL (exp));
    g_object_unref (exp);
    GMenu *section = g_menu_new ();
    g_menu_append (section, "_About Origami Fold", "win.about");
    g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
    g_object_unref (section);

    GtkWidget *menu_btn = gtk_menu_button_new ();
    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_btn),
                                   "open-menu-symbolic");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_btn),
                                    G_MENU_MODEL (menu));
    gtk_widget_set_tooltip_text (menu_btn, "Main Menu");
    g_object_unref (menu);
    adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_btn);

    return header;
}

static GtkWidget *
build_design_view (OrigamiWindow *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *canvas = origami_canvas_new ();
    gtk_widget_set_hexpand (canvas, TRUE);
    gtk_widget_set_vexpand (canvas, TRUE);
    gtk_widget_set_margin_start  (canvas, 12);
    gtk_widget_set_margin_end    (canvas, 12);
    gtk_widget_set_margin_top    (canvas, 12);
    gtk_widget_set_margin_bottom (canvas, 6);
    gtk_box_append (GTK_BOX (box), canvas);
    self->canvas = ORIGAMI_CANVAS (canvas);

    GtkWidget *status = gtk_label_new ("");
    gtk_widget_add_css_class (status, "dim-label");
    gtk_widget_add_css_class (status, "caption");
    gtk_label_set_xalign (GTK_LABEL (status), 0.5);
    gtk_label_set_wrap   (GTK_LABEL (status), TRUE);
    gtk_widget_set_margin_top    (status, 4);
    gtk_widget_set_margin_bottom (status, 12);
    gtk_widget_set_margin_start  (status, 12);
    gtk_widget_set_margin_end    (status, 12);
    gtk_box_append (GTK_BOX (box), status);
    self->status_label = GTK_LABEL (status);

    return box;
}

static GtkWidget *
build_3d_view (OrigamiWindow *self)
{
    GtkWidget *c = origami_canvas3d_new ();
    self->canvas3d = ORIGAMI_CANVAS3D (c);
    gtk_widget_set_hexpand (c, TRUE);
    gtk_widget_set_vexpand (c, TRUE);
    return c;
}

/* ---------- crease tab ---------- */

static void
on_crease_changed (OrigamiCreaseCanvas *cc, OrigamiWindow *self)
{
    origami_crease_canvas3d_set_pattern (
        self->crease_canvas3d,
        origami_crease_canvas_get_pattern (cc));
    origami_crease_canvas3d_redraw (self->crease_canvas3d);
}

static void
on_completion_changed (GtkRange *range, OrigamiWindow *self)
{
    origami_crease_canvas3d_set_completion (self->crease_canvas3d,
                                            gtk_range_get_value (range));
}

static void
on_m_toggled (GtkToggleButton *btn, OrigamiWindow *self)
{
    if (gtk_toggle_button_get_active (btn))
        origami_crease_canvas_set_assignment (self->crease_canvas, CR_MOUNTAIN);
}

static void
on_v_toggled (GtkToggleButton *btn, OrigamiWindow *self)
{
    if (gtk_toggle_button_get_active (btn))
        origami_crease_canvas_set_assignment (self->crease_canvas, CR_VALLEY);
}

static void
on_crease_reset (GtkButton *btn, OrigamiWindow *self)
{
    origami_crease_canvas_reset (self->crease_canvas);
}

static void
on_crease_undo (GtkButton *btn, OrigamiWindow *self)
{
    origami_crease_canvas_undo (self->crease_canvas);
}

static void
on_load_crane (GtkButton *btn, OrigamiWindow *self)
{
    origami_crease_canvas_load_crane (self->crease_canvas);
    /* Park the slider at "unfolded" so the user can drag through the
     * collapse and see the bird-base form. */
    if (self->completion_scale)
        gtk_range_set_value (GTK_RANGE (self->completion_scale), 0.0);
}

static GtkWidget *
build_crease_view (OrigamiWindow *self)
{
    GtkWidget *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    /* Mini-toolbar above the editor. */
    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start  (bar, 12);
    gtk_widget_set_margin_end    (bar, 12);
    gtk_widget_set_margin_top    (bar,  8);
    gtk_widget_set_margin_bottom (bar,  4);

    GtkWidget *reset = gtk_button_new_from_icon_name ("document-new-symbolic");
    gtk_widget_set_tooltip_text (reset, "New crease pattern");
    g_signal_connect (reset, "clicked", G_CALLBACK (on_crease_reset), self);
    gtk_box_append (GTK_BOX (bar), reset);

    GtkWidget *undo = gtk_button_new_from_icon_name ("edit-undo-symbolic");
    gtk_widget_set_tooltip_text (undo, "Undo last crease");
    g_signal_connect (undo, "clicked", G_CALLBACK (on_crease_undo), self);
    gtk_box_append (GTK_BOX (bar), undo);

    GtkWidget *crane = gtk_button_new_with_label ("Crane");
    gtk_widget_set_tooltip_text (crane,
        "Load the bird-base crease pattern — drag the completion slider "
        "to fold it into a crane");
    gtk_widget_add_css_class (crane, "suggested-action");
    g_signal_connect (crane, "clicked", G_CALLBACK (on_load_crane), self);
    gtk_box_append (GTK_BOX (bar), crane);

    /* M / V toggle group. */
    GtkWidget *mv = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class (mv, "linked");
    GtkWidget *mb = gtk_toggle_button_new_with_label ("Mountain");
    gtk_widget_set_tooltip_text (mb, "Next fold = mountain (key M)");
    g_signal_connect (mb, "toggled", G_CALLBACK (on_m_toggled), self);
    gtk_box_append (GTK_BOX (mv), mb);
    GtkWidget *vb = gtk_toggle_button_new_with_label ("Valley");
    gtk_widget_set_tooltip_text (vb, "Next fold = valley (key V)");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (vb), TRUE);
    gtk_toggle_button_set_group  (GTK_TOGGLE_BUTTON (vb), GTK_TOGGLE_BUTTON (mb));
    g_signal_connect (vb, "toggled", G_CALLBACK (on_v_toggled), self);
    gtk_box_append (GTK_BOX (mv), vb);
    gtk_box_append (GTK_BOX (bar), mv);
    self->m_btn = GTK_TOGGLE_BUTTON (mb);
    self->v_btn = GTK_TOGGLE_BUTTON (vb);

    /* Completion slider for the 3D viewer. */
    GtkWidget *spacer = gtk_label_new (NULL);
    gtk_widget_set_hexpand (spacer, TRUE);
    gtk_box_append (GTK_BOX (bar), spacer);

    GtkWidget *cl = gtk_label_new ("Fold completion");
    gtk_widget_add_css_class (cl, "dim-label");
    gtk_widget_add_css_class (cl, "caption");
    gtk_box_append (GTK_BOX (bar), cl);

    GtkWidget *scale = gtk_scale_new_with_range (GTK_ORIENTATION_HORIZONTAL,
                                                 0.0, 1.0, 0.01);
    gtk_range_set_value (GTK_RANGE (scale), 1.0);
    gtk_widget_set_size_request (scale, 200, -1);
    g_signal_connect (scale, "value-changed",
                      G_CALLBACK (on_completion_changed), self);
    gtk_box_append (GTK_BOX (bar), scale);
    self->completion_scale = GTK_SCALE (scale);

    gtk_box_append (GTK_BOX (outer), bar);

    /* Side-by-side editor + 3D preview. */
    GtkWidget *split = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (split), 600);
    gtk_widget_set_hexpand (split, TRUE);
    gtk_widget_set_vexpand (split, TRUE);

    GtkWidget *cc = origami_crease_canvas_new ();
    self->crease_canvas = ORIGAMI_CREASE_CANVAS (cc);
    g_signal_connect (cc, "changed", G_CALLBACK (on_crease_changed), self);
    gtk_paned_set_start_child (GTK_PANED (split), cc);

    GtkWidget *cc3 = origami_crease_canvas3d_new ();
    self->crease_canvas3d = ORIGAMI_CREASE_CANVAS3D (cc3);
    origami_crease_canvas3d_set_pattern (self->crease_canvas3d,
        origami_crease_canvas_get_pattern (self->crease_canvas));
    gtk_paned_set_end_child (GTK_PANED (split), cc3);

    gtk_box_append (GTK_BOX (outer), split);

    /* Hint footer. */
    GtkWidget *hint = gtk_label_new (
        "Click two points to drop a crease. M = mountain, V = valley. "
        "Right-click cancels. Drag the 3D pane to orbit, scroll to zoom.");
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_widget_add_css_class (hint, "caption");
    gtk_label_set_wrap   (GTK_LABEL (hint), TRUE);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.5);
    gtk_widget_set_margin_top    (hint, 4);
    gtk_widget_set_margin_bottom (hint, 8);
    gtk_box_append (GTK_BOX (outer), hint);

    return outer;
}

static GtkWidget *
build_section_label (const char *text)
{
    GtkWidget *l = gtk_label_new (text);
    gtk_widget_add_css_class (l, "heading");
    gtk_label_set_xalign (GTK_LABEL (l), 0.0);
    gtk_widget_set_margin_top    (l, 12);
    gtk_widget_set_margin_bottom (l,  4);
    gtk_widget_set_margin_start  (l, 12);
    gtk_widget_set_margin_end    (l, 12);
    return l;
}

static GtkSpinButton *
build_mm_spin (double initial)
{
    GtkAdjustment *adj = gtk_adjustment_new (initial, 0, PAPER_W_MM, 1, 10, 0);
    GtkWidget *s = gtk_spin_button_new (adj, 1.0, 1);
    gtk_widget_set_hexpand (s, TRUE);
    return GTK_SPIN_BUTTON (s);
}

static GtkWidget *
build_layers_section (OrigamiWindow *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *hdr_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top    (hdr_box, 12);
    gtk_widget_set_margin_bottom (hdr_box,  4);
    gtk_widget_set_margin_start  (hdr_box, 12);
    gtk_widget_set_margin_end    (hdr_box, 12);

    GtkWidget *hdr = gtk_label_new ("Layers");
    gtk_widget_add_css_class (hdr, "heading");
    gtk_label_set_xalign (GTK_LABEL (hdr), 0.0);
    gtk_widget_set_hexpand (hdr, TRUE);
    gtk_box_append (GTK_BOX (hdr_box), hdr);

    GtkWidget *clear = gtk_button_new_with_label ("Clear");
    gtk_widget_set_tooltip_text (clear,
        "Clear selection — folds will then apply to every surface");
    gtk_widget_add_css_class (clear, "flat");
    g_signal_connect (clear, "clicked", G_CALLBACK (on_clear_target), self);
    gtk_box_append (GTK_BOX (hdr_box), clear);
    gtk_box_append (GTK_BOX (box), hdr_box);

    GtkWidget *target = gtk_label_new (
        "No selection — folds apply to all surfaces");
    gtk_widget_add_css_class (target, "dim-label");
    gtk_widget_add_css_class (target, "caption");
    gtk_label_set_xalign (GTK_LABEL (target), 0.0);
    gtk_widget_set_margin_start  (target, 12);
    gtk_widget_set_margin_end    (target, 12);
    gtk_widget_set_margin_bottom (target, 4);
    gtk_box_append (GTK_BOX (box), target);
    self->target_label = GTK_LABEL (target);

    GtkWidget *list = gtk_list_box_new ();
    gtk_widget_add_css_class (list, "boxed-list");
    gtk_widget_set_margin_start  (list, 12);
    gtk_widget_set_margin_end    (list, 12);
    gtk_widget_set_margin_bottom (list,  4);
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_MULTIPLE);
    g_signal_connect (list, "row-selected",
                      G_CALLBACK (on_layer_row_selected), self);
    g_signal_connect (list, "selected-rows-changed",
                      G_CALLBACK (on_layer_rows_changed), self);
    gtk_box_append (GTK_BOX (box), list);
    self->layers_list = GTK_LIST_BOX (list);

    return box;
}

static GtkWidget *
build_coords_section (OrigamiWindow *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append (GTK_BOX (box), build_section_label ("Fold by coordinates (mm)"));

    GtkWidget *grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid), 6);
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 4);
    gtk_widget_set_margin_start  (grid, 12);
    gtk_widget_set_margin_end    (grid, 12);

    GtkWidget *lx1 = gtk_label_new ("x1");  gtk_widget_add_css_class (lx1, "dim-label");
    GtkWidget *ly1 = gtk_label_new ("y1");  gtk_widget_add_css_class (ly1, "dim-label");
    GtkWidget *lx2 = gtk_label_new ("x2");  gtk_widget_add_css_class (lx2, "dim-label");
    GtkWidget *ly2 = gtk_label_new ("y2");  gtk_widget_add_css_class (ly2, "dim-label");

    self->x1_spin = build_mm_spin (0);
    self->y1_spin = build_mm_spin (0);
    self->x2_spin = build_mm_spin (PAPER_W_MM);
    self->y2_spin = build_mm_spin (PAPER_W_MM);

    gtk_grid_attach (GTK_GRID (grid), lx1, 0, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), GTK_WIDGET (self->x1_spin), 1, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), ly1, 2, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), GTK_WIDGET (self->y1_spin), 3, 0, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), lx2, 0, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), GTK_WIDGET (self->x2_spin), 1, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), ly2, 2, 1, 1, 1);
    gtk_grid_attach (GTK_GRID (grid), GTK_WIDGET (self->y2_spin), 3, 1, 1, 1);
    gtk_box_append (GTK_BOX (box), grid);

    GtkWidget *btn_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class (btn_box, "linked");
    gtk_widget_set_margin_start  (btn_box, 12);
    gtk_widget_set_margin_end    (btn_box, 12);
    gtk_widget_set_margin_top    (btn_box,  2);
    gtk_widget_set_margin_bottom (btn_box,  4);

    GtkWidget *bl = gtk_button_new_with_label ("Fold left side");
    gtk_widget_set_hexpand (bl, TRUE);
    g_signal_connect (bl, "clicked", G_CALLBACK (on_fold_coords_left), self);
    gtk_box_append (GTK_BOX (btn_box), bl);

    GtkWidget *br = gtk_button_new_with_label ("Fold right side");
    gtk_widget_set_hexpand (br, TRUE);
    g_signal_connect (br, "clicked", G_CALLBACK (on_fold_coords_right), self);
    gtk_box_append (GTK_BOX (btn_box), br);
    gtk_box_append (GTK_BOX (box), btn_box);

    GtkWidget *hint = gtk_label_new (
        "Origin (0,0) is the top-left of the paper.\n"
        "“Left/right side” = which half of the line is folded over.");
    gtk_widget_add_css_class (hint, "dim-label");
    gtk_widget_add_css_class (hint, "caption");
    gtk_label_set_wrap   (GTK_LABEL (hint), TRUE);
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_set_margin_start  (hint, 12);
    gtk_widget_set_margin_end    (hint, 12);
    gtk_widget_set_margin_bottom (hint,  4);
    gtk_box_append (GTK_BOX (box), hint);

    return box;
}

static GtkWidget *
build_steps_section (OrigamiWindow *self)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (box), build_section_label ("Steps"));

    GtkWidget *list = gtk_list_box_new ();
    gtk_widget_add_css_class (list, "navigation-sidebar");
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list), GTK_SELECTION_NONE);
    g_signal_connect (list, "row-activated",
                      G_CALLBACK (on_step_activated), self);
    gtk_box_append (GTK_BOX (box), list);
    self->steps_list = GTK_LIST_BOX (list);

    return box;
}

static GtkWidget *
build_sidebar (OrigamiWindow *self)
{
    GtkWidget *root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

    GtkWidget *header = adw_header_bar_new ();
    AdwWindowTitle *title = ADW_WINDOW_TITLE (
        adw_window_title_new ("Fold panel", NULL));
    adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                     GTK_WIDGET (title));
    adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
    adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
    gtk_box_append (GTK_BOX (root), header);

    GtkWidget *scroll = gtk_scrolled_window_new ();
    gtk_widget_set_vexpand (scroll, TRUE);
    GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append (GTK_BOX (content), build_layers_section (self));
    gtk_box_append (GTK_BOX (content), build_coords_section (self));
    gtk_box_append (GTK_BOX (content), build_steps_section (self));
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroll), content);
    gtk_box_append (GTK_BOX (root), scroll);

    GtkWidget *exp_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top    (exp_box, 6);
    gtk_widget_set_margin_bottom (exp_box, 8);
    gtk_widget_set_margin_start  (exp_box, 8);
    gtk_widget_set_margin_end    (exp_box, 8);
    GtkWidget *exp_btn = gtk_button_new_with_label ("Export instructions");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (exp_btn), "win.export-md");
    gtk_widget_set_hexpand (exp_btn, TRUE);
    gtk_widget_add_css_class (exp_btn, "suggested-action");
    gtk_box_append (GTK_BOX (exp_box), exp_btn);
    gtk_box_append (GTK_BOX (root), exp_box);

    return root;
}

static void
origami_window_init (OrigamiWindow *self)
{
    /* Build views first because the header bar's view switcher needs the
     * AdwViewStack widget. */
    AdwViewStack *stack = ADW_VIEW_STACK (adw_view_stack_new ());
    adw_view_stack_add_titled_with_icon (stack, build_design_view (self),
        "design", "Design", "applications-graphics-symbolic");
    adw_view_stack_add_titled_with_icon (stack, build_3d_view (self),
        "preview", "3D Preview", "view-paged-symbolic");
    adw_view_stack_add_titled_with_icon (stack, build_crease_view (self),
        "crease", "Crease", "view-grid-symbolic");
    self->view_stack = stack;

    /* Toolbar / header. */
    GtkWidget *header = build_header (self);
    AdwViewSwitcher *switcher = ADW_VIEW_SWITCHER (
        g_object_get_data (G_OBJECT (header), "switcher"));
    adw_view_switcher_set_stack (switcher, stack);

    /* Sidebar split. */
    AdwOverlaySplitView *split = ADW_OVERLAY_SPLIT_VIEW (
        adw_overlay_split_view_new ());
    adw_overlay_split_view_set_sidebar_position (split, GTK_PACK_END);
    adw_overlay_split_view_set_min_sidebar_width (split, 240);
    adw_overlay_split_view_set_max_sidebar_width (split, 360);
    adw_overlay_split_view_set_sidebar_width_fraction (split, 0.28);
    adw_overlay_split_view_set_show_sidebar (split, TRUE);
    adw_overlay_split_view_set_content (split, GTK_WIDGET (stack));
    adw_overlay_split_view_set_sidebar (split, build_sidebar (self));
    self->split = split;

    g_signal_connect (split, "notify::show-sidebar",
                      G_CALLBACK (on_split_show_sidebar), self);

    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    AdwToastOverlay *toast = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
    adw_toast_overlay_set_child (toast, GTK_WIDGET (split));
    self->toast_overlay = toast;

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view),
                                  GTK_WIDGET (toast));
    adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                        toolbar_view);

    g_action_map_add_action_entries (G_ACTION_MAP (self),
                                     win_actions,
                                     G_N_ELEMENTS (win_actions),
                                     self);

    g_signal_connect (self->canvas, "state-changed",
                      G_CALLBACK (on_canvas_state_changed), self);
    g_signal_connect (self->canvas, "history-changed",
                      G_CALLBACK (on_history_changed), self);

    /* Wire the 3D preview to the same paper. */
    origami_canvas3d_set_paper (self->canvas3d,
                                origami_canvas_get_paper (self->canvas));

    rebuild_steps  (self);
    rebuild_layers (self);
    update_status  (self);

    gtk_window_set_title         (GTK_WINDOW (self), "Origami Fold");
    gtk_window_set_default_size  (GTK_WINDOW (self), 1080, 820);
}

static void
origami_window_class_init (OrigamiWindowClass *klass)
{
    (void) klass;
}

OrigamiWindow *
origami_window_new (AdwApplication *app)
{
    return g_object_new (ORIGAMI_TYPE_WINDOW,
                         "application", app,
                         NULL);
}
