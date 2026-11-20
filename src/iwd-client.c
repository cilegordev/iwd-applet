#include "iwd-client.h"
#include <string.h>

#define IWD_SERVICE "net.connman.iwd"

/* Can be overridden at build time to test against a mock iwd on the session bus. */
#ifndef IWD_BUS_TYPE
#define IWD_BUS_TYPE G_BUS_TYPE_SYSTEM
#endif
#define IF_DEVICE   "net.connman.iwd.Device"
#define IF_STATION  "net.connman.iwd.Station"
#define IF_NETWORK  "net.connman.iwd.Network"
#define IF_KNOWN    "net.connman.iwd.KnownNetwork"

#define UPDATE_DELAY_MS 150

struct _IwdClient {
  IwdClientChangedFunc changed_cb;
  IwdClientErrorFunc error_cb;
  gpointer data;

  guint watch_id;
  GDBusObjectManager *mgr;
  GDBusProxy *device;
  GDBusProxy *station;
  GPtrArray *networks;
  GCancellable *fetch_cancel;
  guint update_id;
  IwdState state;
};

static void network_free(gpointer p)
{
  IwdNetwork *n = p;
  g_free(n->path);
  g_free(n->name);
  g_free(n->type);
  g_free(n->known_path);
  g_free(n);
}

static gchar *prop_string(GDBusProxy *p, const char *name)
{
  GVariant *v = g_dbus_proxy_get_cached_property(p, name);
  gchar *s = NULL;
  if (v) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ||
        g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
      s = g_variant_dup_string(v, NULL);
    g_variant_unref(v);
  }
  return s;
}

static gboolean prop_bool(GDBusProxy *p, const char *name)
{
  GVariant *v = g_dbus_proxy_get_cached_property(p, name);
  gboolean b = FALSE;
  if (v) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
      b = g_variant_get_boolean(v);
    g_variant_unref(v);
  }
  return b;
}

static void notify_changed(IwdClient *c)
{
  if (c->changed_cb)
    c->changed_cb(c, c->data);
}

/* The caller owns `err` and frees it. */
static void report_error(IwdClient *c, GError *err)
{
  if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    return;

  gchar *remote = g_dbus_error_get_remote_error(err);
  gboolean quiet = remote &&
    (g_str_equal(remote, "net.connman.iwd.Aborted") ||
     g_str_equal(remote, "net.connman.iwd.Busy"));
  g_free(remote);
  if (quiet)
    return;

  g_dbus_error_strip_remote_error(err);
  if (c->error_cb)
    c->error_cb(c, err->message, c->data);
}

static IwdState map_station_state(const char *s)
{
  if (!s) return IWD_STATE_DISCONNECTED;
  if (g_str_equal(s, "connected") || g_str_equal(s, "roaming")) return IWD_STATE_CONNECTED;
  if (g_str_equal(s, "connecting")) return IWD_STATE_CONNECTING;
  return IWD_STATE_DISCONNECTED; /* "disconnected", "disconnecting" */
}

static void on_networks(GObject *src, GAsyncResult *res, gpointer user_data)
{
  GError *err = NULL;
  GVariant *ret = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);

  /* Don't touch `c` before checking for cancellation: the client may already be freed. */
  if (!ret && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_error_free(err);
    return;
  }

  IwdClient *c = user_data;
  g_ptr_array_set_size(c->networks, 0);

  if (!ret) {
    g_warning("GetOrderedNetworks failed: %s", err->message);
    g_error_free(err);
    notify_changed(c);
    return;
  }

  if (c->mgr) {
    GVariantIter *it = NULL;
    const gchar *path = NULL;
    gint16 sig = 0;

    g_variant_get(ret, "(a(on))", &it);
    while (g_variant_iter_next(it, "(&on)", &path, &sig)) {
      GDBusProxy *np = (GDBusProxy *) g_dbus_object_manager_get_interface(c->mgr, path, IF_NETWORK);
      if (!np)
        continue;

      IwdNetwork *n = g_new0(IwdNetwork, 1);
      n->path = g_strdup(path);
      n->name = prop_string(np, "Name");
      n->type = prop_string(np, "Type");
      n->known_path = prop_string(np, "KnownNetwork");
      n->connected = prop_bool(np, "Connected");
      n->signal_dbm = sig / 100; /* iwd: 100 * dBm */
      g_ptr_array_add(c->networks, n);
      g_object_unref(np);
    }
    g_variant_iter_free(it);
  }

  g_variant_unref(ret);
  notify_changed(c);
}

static gboolean do_update(gpointer user_data)
{
  IwdClient *c = user_data;
  c->update_id = 0;

  g_clear_object(&c->device);
  g_clear_object(&c->station);

  if (c->fetch_cancel) {
    g_cancellable_cancel(c->fetch_cancel);
    g_clear_object(&c->fetch_cancel);
  }

  if (!c->mgr) {
    c->state = IWD_STATE_NO_DAEMON;
    g_ptr_array_set_size(c->networks, 0);
    notify_changed(c);
    return G_SOURCE_REMOVE;
  }

  /* Pick a device, preferring one that has the Station interface. */
  GList *objs = g_dbus_object_manager_get_objects(c->mgr);
  for (GList *l = objs; l; l = l->next) {
    GDBusObject *obj = l->data;
    GDBusProxy *dev = (GDBusProxy *) g_dbus_object_get_interface(obj, IF_DEVICE);
    if (!dev)
      continue;
    GDBusProxy *st = (GDBusProxy *) g_dbus_object_get_interface(obj, IF_STATION);

    if (!c->device || (st && !c->station)) {
      g_clear_object(&c->device);
      g_clear_object(&c->station);
      c->device = dev;
      c->station = st;
    } else {
      g_object_unref(dev);
      if (st)
        g_object_unref(st);
    }
  }
  g_list_free_full(objs, g_object_unref);

  if (!c->device) {
    c->state = IWD_STATE_NO_DEVICE;
  } else if (!prop_bool(c->device, "Powered")) {
    c->state = IWD_STATE_OFF;
  } else if (!c->station) {
    c->state = IWD_STATE_NO_DEVICE; /* powered, but not in station mode */
  } else {
    gchar *s = prop_string(c->station, "State");
    c->state = map_station_state(s);
    g_free(s);
  }

  if (c->station && c->state >= IWD_STATE_DISCONNECTED) {
    c->fetch_cancel = g_cancellable_new();
    g_dbus_proxy_call(c->station, "GetOrderedNetworks", NULL,
                      G_DBUS_CALL_FLAGS_NONE, -1, c->fetch_cancel,
                      on_networks, c);
  } else {
    g_ptr_array_set_size(c->networks, 0);
    notify_changed(c);
  }

  return G_SOURCE_REMOVE;
}

static void schedule_update(IwdClient *c)
{
  if (!c->update_id)
    c->update_id = g_timeout_add(UPDATE_DELAY_MS, do_update, c);
}

/* --- ObjectManager signals --- */

static void on_object_changed(GDBusObjectManager *m, GDBusObject *o, gpointer data)
{
  schedule_update(data);
}

static void on_interface_changed(GDBusObjectManager *m, GDBusObject *o, GDBusInterface *i, gpointer data)
{
  schedule_update(data);
}

static void on_props_changed(GDBusObjectManagerClient *m, GDBusObjectProxy *o, GDBusProxy *p,
                             GVariant *changed, const gchar *const *invalidated, gpointer data)
{
  schedule_update(data);
}

static void drop_manager(IwdClient *c)
{
  if (c->mgr) {
    g_signal_handlers_disconnect_by_data(c->mgr, c);
    g_clear_object(&c->mgr);
  }
}

static void on_name_appeared(GDBusConnection *conn, const gchar *name, const gchar *owner, gpointer data)
{
  IwdClient *c = data;
  GError *err = NULL;

  drop_manager(c);
  c->mgr = g_dbus_object_manager_client_new_sync(
      conn, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE,
      IWD_SERVICE, "/", NULL, NULL, NULL, NULL, &err);
  if (!c->mgr) {
    g_warning("Failed to create the iwd ObjectManager: %s", err->message);
    g_error_free(err);
  } else {
    g_signal_connect(c->mgr, "object-added", G_CALLBACK(on_object_changed), c);
    g_signal_connect(c->mgr, "object-removed", G_CALLBACK(on_object_changed), c);
    g_signal_connect(c->mgr, "interface-added", G_CALLBACK(on_interface_changed), c);
    g_signal_connect(c->mgr, "interface-removed", G_CALLBACK(on_interface_changed), c);
    g_signal_connect(c->mgr, "interface-proxy-properties-changed", G_CALLBACK(on_props_changed), c);
  }
  schedule_update(c);
}

static void on_name_vanished(GDBusConnection *conn, const gchar *name, gpointer data)
{
  IwdClient *c = data;
  drop_manager(c);
  schedule_update(c);
}

/* --- public API --- */

IwdClient *iwd_client_new(IwdClientChangedFunc changed, IwdClientErrorFunc error, gpointer user_data)
{
  IwdClient *c = g_new0(IwdClient, 1);
  c->changed_cb = changed;
  c->error_cb = error;
  c->data = user_data;
  c->state = IWD_STATE_NO_DAEMON;
  c->networks = g_ptr_array_new_with_free_func(network_free);
  c->watch_id = g_bus_watch_name(IWD_BUS_TYPE, IWD_SERVICE,
                                 G_BUS_NAME_WATCHER_FLAGS_NONE,
                                 on_name_appeared, on_name_vanished, c, NULL);
  return c;
}

void iwd_client_free(IwdClient *c)
{
  if (!c)
    return;
  if (c->update_id)
    g_source_remove(c->update_id);
  if (c->fetch_cancel) {
    g_cancellable_cancel(c->fetch_cancel);
    g_object_unref(c->fetch_cancel);
  }
  g_bus_unwatch_name(c->watch_id);
  drop_manager(c);
  g_clear_object(&c->device);
  g_clear_object(&c->station);
  g_ptr_array_unref(c->networks);
  g_free(c);
}

IwdState iwd_client_get_state(IwdClient *c) { return c->state; }
GPtrArray *iwd_client_get_networks(IwdClient *c) { return c->networks; }

const char *iwd_client_get_connected_name(IwdClient *c)
{
  for (guint i = 0; i < c->networks->len; i++) {
    IwdNetwork *n = g_ptr_array_index(c->networks, i);
    if (n->connected)
      return n->name;
  }
  return NULL;
}

int iwd_client_get_connected_signal(IwdClient *c)
{
  for (guint i = 0; i < c->networks->len; i++) {
    IwdNetwork *n = g_ptr_array_index(c->networks, i);
    if (n->connected)
      return n->signal_dbm;
  }
  return -100;
}

gboolean iwd_client_is_scanning(IwdClient *c)
{
  return c->station && prop_bool(c->station, "Scanning");
}

char *iwd_client_network_name(IwdClient *c, const char *network_path)
{
  if (!c->mgr)
    return NULL;
  GDBusProxy *np = (GDBusProxy *) g_dbus_object_manager_get_interface(c->mgr, network_path, IF_NETWORK);
  if (!np)
    return NULL;
  gchar *name = prop_string(np, "Name");
  g_object_unref(np);
  return name;
}

static void call_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IwdClient *c = user_data;
  GError *err = NULL;
  GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(src), res, &err);
  if (r) {
    g_variant_unref(r);
  } else {
    report_error(c, err);
    g_error_free(err);
  }
}

static void conn_call_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IwdClient *c = user_data;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
  if (r) {
    g_variant_unref(r);
  } else {
    report_error(c, err);
    g_error_free(err);
  }
}

void iwd_client_scan(IwdClient *c)
{
  if (c->station)
    g_dbus_proxy_call(c->station, "Scan", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, call_done, c);
}

void iwd_client_disconnect(IwdClient *c)
{
  if (c->station)
    g_dbus_proxy_call(c->station, "Disconnect", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, call_done, c);
}

void iwd_client_connect(IwdClient *c, const char *network_path)
{
  if (!c->mgr)
    return;
  GDBusProxy *np = (GDBusProxy *) g_dbus_object_manager_get_interface(c->mgr, network_path, IF_NETWORK);
  if (!np)
    return;
  /* Connect blocks until it finishes (including the passphrase prompt), so use a long timeout. */
  g_dbus_proxy_call(np, "Connect", NULL, G_DBUS_CALL_FLAGS_NONE, 120000, NULL, call_done, c);
  g_object_unref(np);
}

void iwd_client_connect_hidden(IwdClient *c, const char *ssid)
{
  if (c->station)
    g_dbus_proxy_call(c->station, "ConnectHiddenNetwork", g_variant_new("(s)", ssid),
                      G_DBUS_CALL_FLAGS_NONE, 120000, NULL, call_done, c);
}

void iwd_client_forget(IwdClient *c, const char *known_path)
{
  if (!c->mgr || !known_path)
    return;
  GDBusProxy *kp = (GDBusProxy *) g_dbus_object_manager_get_interface(c->mgr, known_path, IF_KNOWN);
  if (!kp)
    return;
  g_dbus_proxy_call(kp, "Forget", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, call_done, c);
  g_object_unref(kp);
}

void iwd_client_set_powered(IwdClient *c, gboolean powered)
{
  if (!c->device)
    return;
  g_dbus_connection_call(g_dbus_proxy_get_connection(c->device),
                         IWD_SERVICE, g_dbus_proxy_get_object_path(c->device),
                         "org.freedesktop.DBus.Properties", "Set",
                         g_variant_new("(ssv)", IF_DEVICE, "Powered", g_variant_new_boolean(powered)),
                         NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, conn_call_done, c);
}
