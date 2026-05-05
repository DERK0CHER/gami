#include <adwaita.h>

#include "window.h"

static void
on_activate (GApplication *app, gpointer user_data)
{
    GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (!win)
        win = GTK_WINDOW (origami_window_new (ADW_APPLICATION (app)));
    gtk_window_present (win);
}

int
main (int argc, char *argv[])
{
    g_set_application_name ("Origami Fold");

    AdwApplication *app = adw_application_new (
        "org.gnome.example.OrigamiFold",
        G_APPLICATION_DEFAULT_FLAGS);

    g_signal_connect (app, "activate", G_CALLBACK (on_activate), NULL);

    GtkApplication *gtk_app = GTK_APPLICATION (app);
    const char *undo_keys[]   = { "<Control>z",        NULL };
    const char *new_keys[]    = { "<Control>n",        NULL };
    const char *cancel_keys[] = { "Escape",            NULL };
    const char *close_keys[]  = { "<Control>w",        NULL };
    const char *about_keys[]  = { "F1",                NULL };
    gtk_application_set_accels_for_action (gtk_app, "win.undo",     undo_keys);
    gtk_application_set_accels_for_action (gtk_app, "win.new",      new_keys);
    gtk_application_set_accels_for_action (gtk_app, "win.cancel",   cancel_keys);
    gtk_application_set_accels_for_action (gtk_app, "win.about",    about_keys);
    gtk_application_set_accels_for_action (gtk_app, "window.close", close_keys);

    int status = g_application_run (G_APPLICATION (app), argc, argv);
    g_object_unref (app);
    return status;
}
