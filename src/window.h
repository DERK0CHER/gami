#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define ORIGAMI_TYPE_WINDOW (origami_window_get_type ())
G_DECLARE_FINAL_TYPE (OrigamiWindow, origami_window,
                      ORIGAMI, WINDOW, AdwApplicationWindow)

OrigamiWindow *origami_window_new (AdwApplication *app);

G_END_DECLS
