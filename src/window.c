#include "window.h"

#include "canvas.h"

struct _OrigamiWindow {
    AdwApplicationWindow parent_instance;

    OrigamiCanvas    *canvas;
    GtkLabel         *status_label;
    AdwToastOverlay  *toast_overlay;
    GtkButton        *undo_button;
};

G_DEFINE_FINAL_TYPE (OrigamiWindow, origami_window, ADW_TYPE_APPLICATION_WINDOW)

/* ---------- ui state sync ---------- */

static void
update_status (OrigamiWindow *self)
{
    OrigamiCanvasState s = origami_canvas_get_state (self->canvas);
    const char *text;
    switch (s) {
    case ORIGAMI_CANVAS_STATE_FIRST_POINT:
        text = "Click on the paper to place the first point of a fold line.";
        break;
    case ORIGAMI_CANVAS_STATE_SECOND_POINT:
        text = "Click to place the second point. Press Esc or right-click to cancel.";
        break;
    case ORIGAMI_CANVAS_STATE_PICK_SIDE:
        text = "Click on the side of the line you want to fold over.";
        break;
    default:
        text = "";
    }
    gtk_label_set_text (self->status_label, text);

    gtk_widget_set_sensitive (
        GTK_WIDGET (self->undo_button),
        origami_canvas_can_undo (self->canvas));
}

static void
on_canvas_state_changed (OrigamiCanvas *canvas, OrigamiWindow *self)
{
    update_status (self);
}

/* ---------- actions ---------- */

static void
action_new (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    origami_canvas_reset (self->canvas);

    AdwToast *toast = adw_toast_new ("New paper");
    adw_toast_set_timeout (toast, 1);
    adw_toast_overlay_add_toast (self->toast_overlay, toast);
}

static void
action_undo (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    origami_canvas_undo (self->canvas);
}

static void
action_cancel (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    origami_canvas_cancel (self->canvas);
}

static void
action_about (GSimpleAction *action, GVariant *param, gpointer user_data)
{
    OrigamiWindow *self = user_data;
    const char *developers[] = { "Origami Fold contributors", NULL };

    AdwDialog *about = g_object_new (
        ADW_TYPE_ABOUT_DIALOG,
        "application-name",  "Origami Fold",
        "application-icon",  "applications-graphics-symbolic",
        "version",           "0.1.0",
        "developers",        developers,
        "license-type",      GTK_LICENSE_GPL_3_0,
        "comments",
        "Draw a fold line, then click the side you want to fold over.\n"
        "Stack folds to make shapes.",
        NULL);
    adw_dialog_present (about, GTK_WIDGET (self));
}

static const GActionEntry win_actions[] = {
    { "new",    action_new,    NULL, NULL, NULL },
    { "undo",   action_undo,   NULL, NULL, NULL },
    { "cancel", action_cancel, NULL, NULL, NULL },
    { "about",  action_about,  NULL, NULL, NULL },
};

/* ---------- construction ---------- */

static GtkWidget *
build_header (OrigamiWindow *self)
{
    GtkWidget *header = adw_header_bar_new ();

    AdwWindowTitle *title = ADW_WINDOW_TITLE (
        adw_window_title_new ("Origami Fold", NULL));
    adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                     GTK_WIDGET (title));

    GtkWidget *new_btn = gtk_button_new_from_icon_name ("document-new-symbolic");
    gtk_widget_set_tooltip_text (new_btn, "New paper");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (new_btn), "win.new");
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), new_btn);

    GtkWidget *undo_btn = gtk_button_new_from_icon_name ("edit-undo-symbolic");
    gtk_widget_set_tooltip_text (undo_btn, "Undo");
    gtk_actionable_set_action_name (GTK_ACTIONABLE (undo_btn), "win.undo");
    adw_header_bar_pack_start (ADW_HEADER_BAR (header), undo_btn);
    self->undo_button = GTK_BUTTON (undo_btn);

    GMenu *menu = g_menu_new ();
    g_menu_append (menu, "_New Paper",  "win.new");
    g_menu_append (menu, "_Undo",       "win.undo");
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
build_content (OrigamiWindow *self)
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
    gtk_widget_set_margin_top    (status, 4);
    gtk_widget_set_margin_bottom (status, 12);
    gtk_widget_set_margin_start  (status, 12);
    gtk_widget_set_margin_end    (status, 12);
    gtk_box_append (GTK_BOX (box), status);
    self->status_label = GTK_LABEL (status);

    return box;
}

static void
origami_window_init (OrigamiWindow *self)
{
    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                  build_header (self));

    AdwToastOverlay *toast = ADW_TOAST_OVERLAY (adw_toast_overlay_new ());
    adw_toast_overlay_set_child (toast, build_content (self));
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

    update_status (self);

    gtk_window_set_title         (GTK_WINDOW (self), "Origami Fold");
    gtk_window_set_default_size  (GTK_WINDOW (self), 720, 780);
}

static void
origami_window_class_init (OrigamiWindowClass *klass)
{
    /* nothing to override */
    (void) klass;
}

OrigamiWindow *
origami_window_new (AdwApplication *app)
{
    return g_object_new (ORIGAMI_TYPE_WINDOW,
                         "application", app,
                         NULL);
}
