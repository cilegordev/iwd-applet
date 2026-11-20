/* StatusNotifierItem backend implemented directly on GDBus (no libappindicator).
 *   left click  -> Activate -> network menu (our own GtkMenu)
 *   right click -> small dbusmenu (About / Quit) exported through the Menu property
 * ItemIsMenu=false so that hosts call Activate on left click.
 * Note: some trays never call ContextMenu; on right click they only show the item's
 * dbusmenu (or the panel plugin's own menu if the item has no dbusmenu). */
#include "config.h"
#include "tray.h"
#include "menu.h"
#include <gtk/gtk.h>
#include <libdbusmenu-glib/server.h>
#include <libdbusmenu-glib/menuitem.h>
#include <unistd.h>

#define SNI_IFACE     "org.kde.StatusNotifierItem"
#define SNI_PATH      "/StatusNotifierItem"
#define DBUSMENU_PATH "/MenuBar"
#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"

static const char introspection_xml[] =
  "<node><interface name='org.kde.StatusNotifierItem'>"
  "<property name='Category' type='s' access='read'/>"
  "<property name='Id' type='s' access='read'/>"
  "<property name='Title' type='s' access='read'/>"
  "<property name='Status' type='s' access='read'/>"
  "<property name='WindowId' type='u' access='read'/>"
  "<property name='IconName' type='s' access='read'/>"
  "<property name='IconPixmap' type='a(iiay)' access='read'/>"
  "<property name='OverlayIconName' type='s' access='read'/>"
  "<property name='AttentionIconName' type='s' access='read'/>"
  "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
  "<property name='ItemIsMenu' type='b' access='read'/>"
  "<property name='Menu' type='o' access='read'/>"
  "<method name='ContextMenu'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "<method name='Activate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "<method name='SecondaryActivate'><arg type='i' direction='in'/><arg type='i' direction='in'/></method>"
  "<method name='Scroll'><arg type='i' direction='in'/><arg type='s' direction='in'/></method>"
  "<signal name='NewTitle'/><signal name='NewIcon'/><signal name='NewAttentionIcon'/>"
  "<signal name='NewOverlayIcon'/><signal name='NewToolTip'/>"
  "<signal name='NewStatus'><arg type='s'/></signal>"
  "</interface></node>";

struct _Tray {
  IwdClient *client;
  GDBusConnection *conn;
  GDBusNodeInfo *node;
  guint own_id;
  guint reg_id;
  guint watch_id;
  char *bus_name;
  char *icon;
  char *title;
  GtkWidget *main_menu;
  GtkWidget *context_menu;
  DbusmenuServer *dm_server;
  DbusmenuMenuitem *dm_root;
};

static void emit(Tray *t, const char *signal, GVariant *params)
{
  if (t->conn)
    g_dbus_connection_emit_signal(t->conn, NULL, SNI_PATH, SNI_IFACE, signal, params, NULL);
  else if (params)
    g_variant_unref(g_variant_ref_sink(params));
}

/* ---------- properties & methods ---------- */

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

static GVariant *get_property(GDBusConnection *c, const gchar *sender, const gchar *path,
                              const gchar *iface, const gchar *prop, GError **error, gpointer data)
{
  Tray *t = data;

  if (g_str_equal(prop, "Category"))          return g_variant_new_string("Hardware");
  if (g_str_equal(prop, "Id"))                return g_variant_new_string("iwd-applet");
  if (g_str_equal(prop, "Title"))             return g_variant_new_string(t->title ? t->title : "iwd-applet");
  if (g_str_equal(prop, "Status"))            return g_variant_new_string("Active");
  if (g_str_equal(prop, "WindowId"))          return g_variant_new_uint32(0);
  if (g_str_equal(prop, "IconName"))          return g_variant_new_string(t->icon ? t->icon : "network-wireless-offline-symbolic");
  if (g_str_equal(prop, "IconPixmap"))        return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
  if (g_str_equal(prop, "OverlayIconName"))   return g_variant_new_string("");
  if (g_str_equal(prop, "AttentionIconName")) return g_variant_new_string("");
  if (g_str_equal(prop, "ItemIsMenu"))        return g_variant_new_boolean(FALSE);
  if (g_str_equal(prop, "Menu"))              return g_variant_new_object_path(DBUSMENU_PATH);
  if (g_str_equal(prop, "ToolTip")) {
    GVariant *pix = g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
    return g_variant_new("(s@a(iiay)ss)", t->icon ? t->icon : "", pix, t->title ? t->title : "", "");
  }

  g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Unknown property: %s", prop);
  return NULL;
}

static void handle_method(GDBusConnection *c, const gchar *sender, const gchar *path,
                          const gchar *iface, const gchar *method, GVariant *params,
                          GDBusMethodInvocation *inv, gpointer data)
{
  Tray *t = data;

  g_dbus_method_invocation_return_value(inv, NULL);

  if (g_str_equal(method, "Activate")) {
    reset_main_menu(t);
    menu_build_main(t->main_menu, t->client, FALSE, TRUE);
    menu_popup_at_pointer(t->main_menu);
  } else if (g_str_equal(method, "ContextMenu")) {
    menu_build_context(t->context_menu);
    menu_popup_at_pointer(t->context_menu);
  }
  /* SecondaryActivate and Scroll are ignored */
}

static const GDBusInterfaceVTable vtable = { handle_method, get_property, NULL, { 0 } };

/* ---------- registration with the StatusNotifierWatcher ---------- */

static void on_registered(GObject *src, GAsyncResult *res, gpointer data)
{
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (r) {
    g_variant_unref(r);
  } else {
    g_warning("RegisterStatusNotifierItem failed: %s", err->message);
    g_error_free(err);
  }
}

static void on_watcher_appeared(GDBusConnection *c, const gchar *name, const gchar *owner, gpointer data)
{
  Tray *t = data;
  g_dbus_connection_call(c, WATCHER_NAME, WATCHER_PATH, WATCHER_IFACE, "RegisterStatusNotifierItem",
                         g_variant_new("(s)", t->bus_name), NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_registered, t);
}

static void on_watcher_vanished(GDBusConnection *c, const gchar *name, gpointer data)
{
  /* No tray host available; we register again automatically when a watcher appears. */
}

static void on_bus_acquired(GDBusConnection *c, const gchar *name, gpointer data)
{
  Tray *t = data;
  GError *err = NULL;

  t->conn = g_object_ref(c);
  t->reg_id = g_dbus_connection_register_object(c, SNI_PATH, t->node->interfaces[0],
                                                &vtable, t, NULL, &err);
  if (!t->reg_id) {
    g_warning("Failed to register the StatusNotifierItem: %s", err->message);
    g_error_free(err);
  }
}

static void on_name_acquired(GDBusConnection *c, const gchar *name, gpointer data)
{
  Tray *t = data;
  t->watch_id = g_bus_watch_name_on_connection(c, WATCHER_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                                               on_watcher_appeared, on_watcher_vanished, t, NULL);
}

static void on_name_lost(GDBusConnection *c, const gchar *name, gpointer data)
{
  g_warning("Could not own bus name %s", name);
}

/* ---------- right-click dbusmenu ---------- */

static void on_dm_about(DbusmenuMenuitem *mi, guint timestamp, gpointer data)
{
  menu_show_about();
}

static void on_dm_quit(DbusmenuMenuitem *mi, guint timestamp, gpointer data)
{
  g_application_quit(g_application_get_default());
}

static void dm_append(DbusmenuMenuitem *root, const char *label, const char *icon, GCallback cb)
{
  DbusmenuMenuitem *mi = dbusmenu_menuitem_new();
  dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_LABEL, label);
  dbusmenu_menuitem_property_set(mi, DBUSMENU_MENUITEM_PROP_ICON_NAME, icon);
  g_signal_connect(mi, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED, cb, NULL);
  dbusmenu_menuitem_child_append(root, mi);
  g_object_unref(mi);
}

static void build_dbusmenu(Tray *t)
{
  t->dm_server = dbusmenu_server_new(DBUSMENU_PATH);
  t->dm_root = dbusmenu_menuitem_new();
  dm_append(t->dm_root, "About", "help-about-symbolic", G_CALLBACK(on_dm_about));
  dm_append(t->dm_root, "Quit", "application-exit-symbolic", G_CALLBACK(on_dm_quit));
  dbusmenu_server_set_root(t->dm_server, t->dm_root);
}

/* ---------- API ---------- */

void tray_update(Tray *t)
{
  char *icon = menu_status_icon(t->client);
  char *title = menu_status_text(t->client);

  gboolean icon_changed = g_strcmp0(icon, t->icon) != 0;
  gboolean title_changed = g_strcmp0(title, t->title) != 0;

  g_free(t->icon);
  g_free(t->title);
  t->icon = icon;
  t->title = title;

  if (icon_changed)
    emit(t, "NewIcon", NULL);
  if (title_changed) {
    emit(t, "NewTitle", NULL);
    emit(t, "NewToolTip", NULL);
  }

  /* Keep an open menu current (e.g. after "Rescan"). Nothing happens if nothing visible changed;
   * if the menu got a different size, grow or shrink it in place. */
  if (gtk_widget_get_visible(t->main_menu) &&
      menu_build_main(t->main_menu, t->client, FALSE, TRUE))
    menu_reposition(t->main_menu);
}

Tray *tray_new(IwdClient *client)
{
  GError *err = NULL;
  Tray *t = g_new0(Tray, 1);
  t->client = client;
  t->main_menu = g_object_ref_sink(gtk_menu_new());
  t->context_menu = g_object_ref_sink(gtk_menu_new());
  t->bus_name = g_strdup_printf("org.kde.StatusNotifierItem-%d-1", (int) getpid());

  t->node = g_dbus_node_info_new_for_xml(introspection_xml, &err);
  if (!t->node) {
    g_warning("Invalid SNI introspection data: %s", err->message);
    g_error_free(err);
    tray_free(t);
    return NULL;
  }

  build_dbusmenu(t);
  tray_update(t);
  t->own_id = g_bus_own_name(G_BUS_TYPE_SESSION, t->bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
                             on_bus_acquired, on_name_acquired, on_name_lost, t, NULL);
  return t;
}

void tray_free(Tray *t)
{
  if (!t)
    return;
  if (t->watch_id)
    g_bus_unwatch_name(t->watch_id);
  if (t->reg_id)
    g_dbus_connection_unregister_object(t->conn, t->reg_id);
  if (t->own_id)
    g_bus_unown_name(t->own_id);
  if (t->node)
    g_dbus_node_info_unref(t->node);
  g_clear_object(&t->dm_root);
  g_clear_object(&t->dm_server);
  g_clear_object(&t->conn);
  g_clear_object(&t->main_menu);
  g_clear_object(&t->context_menu);
  g_free(t->bus_name);
  g_free(t->icon);
  g_free(t->title);
  g_free(t);
}
