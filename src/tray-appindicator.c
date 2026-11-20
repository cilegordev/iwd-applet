/* libappindicator backend: there is only one menu (left and right click behave the same),
 * so "About" and "Quit" go below the network list. */
#include "tray.h"
#include "menu.h"
#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>

struct _Tray {
  IwdClient *client;
  AppIndicator *ind;
  GtkWidget *menu;
};

void tray_update(Tray *t)
{
  char *icon = menu_status_icon(t->client);
  char *status = menu_status_text(t->client);

  app_indicator_set_icon_full(t->ind, icon, status);
  app_indicator_set_title(t->ind, status);
  menu_build_main(t->menu, t->client, TRUE, FALSE);

  g_free(icon);
  g_free(status);
}

Tray *tray_new(IwdClient *client)
{
  Tray *t = g_new0(Tray, 1);
  t->client = client;
  t->ind = app_indicator_new("iwd-applet", "network-wireless-offline-symbolic",
                             APP_INDICATOR_CATEGORY_HARDWARE);
  t->menu = gtk_menu_new();

  tray_update(t);
  app_indicator_set_menu(t->ind, GTK_MENU(t->menu));
  app_indicator_set_status(t->ind, APP_INDICATOR_STATUS_ACTIVE);
  return t;
}

void tray_free(Tray *t)
{
  if (!t)
    return;
  g_object_unref(t->ind);
  g_free(t);
}
