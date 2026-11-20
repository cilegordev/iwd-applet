#pragma once
#include <gtk/gtk.h>
#include "iwd-client.h"

/* Tray icon name for the current state. Free the result with g_free(). */
char *menu_status_icon(IwdClient *c);
/* Status text ("Connected: SSID (-50 dBm)", etc.). Free the result with g_free(). */
char *menu_status_text(IwdClient *c);

/* Clears and refills the main menu (status, network list, actions).
 * with_app_items: append a separator plus About and Quit at the bottom (for single-menu backends).
 * allow_columns: if the list is too tall for the screen, spread the networks over several columns
 *   instead of letting GTK scroll the menu (only for menus drawn by this process).
 * Returns TRUE if the menu was rebuilt and its size changed. If the menu is already open, call
 * menu_reposition() then so that it grows or shrinks to the new content. */
gboolean menu_build_main(GtkWidget *menu, IwdClient *c, gboolean with_app_items, gboolean allow_columns);

/* Right-click menu: About, Quit. */
void menu_build_context(GtkWidget *menu);

void menu_show_about(void);

/* Pops up a menu near the pointer without a real click event (called from SNI D-Bus methods). */
void menu_popup_at_pointer(GtkWidget *menu);

/* Pops a menu up so that its left edge lines up with (x, y) in root coordinates, using the same
 * placement rules as menu_popup_at_pointer(). */
void menu_popup_at(GtkWidget *menu, gint x, gint y);

/* Recomputes size and position of an open menu after its content changed. */
void menu_reposition(GtkWidget *menu);
