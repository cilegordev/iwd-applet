/* GtkStatusIcon (XEmbed) backend.
 *   left click  -> "activate"   -> network menu
 *   right click -> "popup-menu" -> About / Quit
 * Both menus are plain GtkMenus drawn by this process, so they look
 * identical and compact on any panel. The panel does not render the menu, so it cannot add
 * its own spacing. Limitations: X11 only, and GtkStatusIcon is deprecated in GTK 3. */
#include "tray.h"
#include "menu.h"
#include <gtk/gtk.h>

struct _Tray {
  IwdClient *client;
  GtkStatusIcon *icon;
  GtkWidget *main_menu;
  GtkWidget *context_menu;
};

static void popup(Tray *t, GtkWidget *menu, guint button, guint32 time)
{
  gtk_menu_popup(GTK_MENU(menu), NULL, NULL, gtk_status_icon_position_menu, t->icon, button, time);
}

/* Use a brand-new GtkMenu every time the menu is opened. A GtkMenu keeps its previous allocation
 * after it was closed, and GTK uses that stale size to position the next popup; after the content
 * shrank (for example when Wi-Fi was switched off) the menu would end up in the wrong place. */
static void reset_main_menu(Tray *t)
{
  if (t->main_menu) {
    gtk_menu_popdown(GTK_MENU(t->main_menu));
    gtk_widget_destroy(t->main_menu);
    g_object_unref(t->main_menu);
  }
  t->main_menu = g_object_ref_sink(gtk_menu_new());
}

/* The network menu is placed like the one of the sni backend, anchored at the tray icon. This
 * avoids gtk_status_icon_position_menu(), which cannot cope with a menu that grows while open. */
static void on_activate(GtkStatusIcon *icon, gpointer data)
{
  Tray *t = data;
  GdkScreen *screen = NULL;
  GdkRectangle area;
  GtkOrientation orientation;
  gint x = 0, y = 0;

  if (gtk_status_icon_get_geometry(icon, &screen, &area, &orientation)) {
    x = area.x;
    y = area.y + area.height / 2;
  } else {
    gdk_device_get_position(gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default())),
                            NULL, &x, &y);
  }

  reset_main_menu(t);
  menu_build_main(t->main_menu, t->client, FALSE, TRUE);
  menu_popup_at(t->main_menu, x, y);
}

static void on_popup_menu(GtkStatusIcon *icon, guint button, guint32 time, gpointer data)
{
  Tray *t = data;
  menu_build_context(t->context_menu);
  popup(t, t->context_menu, button, time);
}

void tray_update(Tray *t)
{
  char *icon = menu_status_icon(t->client);
  char *title = menu_status_text(t->client);

  gtk_status_icon_set_from_icon_name(t->icon, icon);
  gtk_status_icon_set_tooltip_text(t->icon, title);

  g_free(icon);
  g_free(title);

  /* Keep an open menu current (e.g. after "Rescan"). Nothing happens if nothing visible changed;
   * if the menu got a different size, grow or shrink it in place. */
  if (gtk_widget_get_visible(t->main_menu) &&
      menu_build_main(t->main_menu, t->client, FALSE, TRUE))
    menu_reposition(t->main_menu);
}

Tray *tray_new(IwdClient *client)
{
  Tray *t = g_new0(Tray, 1);
  t->client = client;
  t->main_menu = g_object_ref_sink(gtk_menu_new());
  t->context_menu = g_object_ref_sink(gtk_menu_new());

  t->icon = gtk_status_icon_new();
  gtk_status_icon_set_title(t->icon, "iwd-applet");
  g_signal_connect(t->icon, "activate", G_CALLBACK(on_activate), t);
  g_signal_connect(t->icon, "popup-menu", G_CALLBACK(on_popup_menu), t);

  tray_update(t);
  gtk_status_icon_set_visible(t->icon, TRUE);
  return t;
}

void tray_free(Tray *t)
{
  if (!t)
    return;
  g_clear_object(&t->icon);
  g_clear_object(&t->main_menu);
  g_clear_object(&t->context_menu);
  g_free(t);
}
