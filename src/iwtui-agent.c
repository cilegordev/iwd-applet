#include "iwtui-agent.h"
#include <string.h>

#define IWD_SERVICE  "net.connman.iwd"
#define AGENT_PATH   "/org/iwdapplet/TuiAgent"
#define ERR_CANCELED "net.connman.iwd.Agent.Error.Canceled"

/* Can be overridden at build time to test against a mock iwd on the session bus. */
#ifndef IWD_BUS_TYPE
#define IWD_BUS_TYPE G_BUS_TYPE_SYSTEM
#endif

static const char introspection_xml[] =
  "<node><interface name='net.connman.iwd.Agent'>"
  "<method name='Release'/>"
  "<method name='RequestPassphrase'>"
  "<arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
  "<method name='RequestPrivateKeyPassphrase'>"
  "<arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
  "<method name='RequestUserNameAndPassword'>"
  "<arg type='o' direction='in'/><arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
  "<method name='RequestUserPassword'>"
  "<arg type='o' direction='in'/><arg type='s' direction='in'/><arg type='s' direction='out'/></method>"
  "<method name='Cancel'><arg type='s' direction='in'/></method>"
  "</interface></node>";

struct _TuiAgent {
  GDBusConnection *conn;
  GDBusNodeInfo *node;
  guint reg_id;
  gboolean registered;
  gboolean registering;
  gboolean attempted; /* one registration attempt per daemon start */
  gchar *error;
  gchar *path;
  gchar *passphrase;
  TuiAgentChangedFunc changed;
  gpointer data;
};

static void wipe(gchar *s)
{
  if (s)
    explicit_bzero(s, strlen(s));
}

static void notify_changed(TuiAgent *a)
{
  if (a->changed)
    a->changed(a->data);
}

static void handle_method(GDBusConnection *conn, const gchar *sender, const gchar *object_path,
                          const gchar *interface_name, const gchar *method, GVariant *params,
                          GDBusMethodInvocation *inv, gpointer user_data)
{
  TuiAgent *a = user_data;

  if (g_str_equal(method, "Release")) {
    a->registered = FALSE;
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (g_str_equal(method, "Cancel")) {
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (g_str_equal(method, "RequestPassphrase") ||
             g_str_equal(method, "RequestPrivateKeyPassphrase")) {
    const gchar *path = NULL;
    g_variant_get(params, "(&o)", &path);

    if (a->passphrase && a->path && g_str_equal(a->path, path))
      g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", a->passphrase));
    else
      g_dbus_method_invocation_return_dbus_error(inv, ERR_CANCELED, "No passphrase available");
  } else {
    /* 802.1x (username/password) is not supported yet */
    g_dbus_method_invocation_return_dbus_error(inv, ERR_CANCELED, "Not supported yet");
  }
}

static const GDBusInterfaceVTable vtable = { handle_method, NULL, NULL, { 0 } };

TuiAgent *tui_agent_new(TuiAgentChangedFunc changed, gpointer user_data)
{
  GError *err = NULL;
  TuiAgent *a = g_new0(TuiAgent, 1);

  a->changed = changed;
  a->data = user_data;

  a->conn = g_bus_get_sync(IWD_BUS_TYPE, NULL, &err);
  if (!a->conn)
    goto fail;

  a->node = g_dbus_node_info_new_for_xml(introspection_xml, &err);
  if (!a->node)
    goto fail;

  a->reg_id = g_dbus_connection_register_object(a->conn, AGENT_PATH, a->node->interfaces[0],
                                                &vtable, a, NULL, &err);
  if (!a->reg_id)
    goto fail;

  return a;

fail:
  a->error = g_strdup(err ? err->message : "unknown error");
  g_clear_error(&err);
  return a;
}

static void on_registered(GObject *src, GAsyncResult *res, gpointer user_data)
{
  TuiAgent *a = user_data;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  a->registering = FALSE;
  if (r) {
    g_variant_unref(r);
    a->registered = TRUE;
    g_clear_pointer(&a->error, g_free);
  } else {
    g_dbus_error_strip_remote_error(err);
    g_free(a->error);
    a->error = g_strdup(err->message);
    g_error_free(err);
  }
  notify_changed(a);
}

void tui_agent_set_daemon_up(TuiAgent *a, gboolean up)
{
  if (!up) {
    a->registered = FALSE;
    a->registering = FALSE;
    a->attempted = FALSE;
    return;
  }

  if (!a->reg_id || a->attempted)
    return;

  a->attempted = TRUE;
  a->registering = TRUE;
  g_dbus_connection_call(a->conn, IWD_SERVICE, "/net/connman/iwd",
                         "net.connman.iwd.AgentManager", "RegisterAgent",
                         g_variant_new("(o)", AGENT_PATH), NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_registered, a);
}

gboolean tui_agent_is_registered(TuiAgent *a)
{
  return a->registered;
}

const char *tui_agent_error(TuiAgent *a)
{
  return a->error;
}

void tui_agent_set_passphrase(TuiAgent *a, const char *network_path, const char *passphrase)
{
  wipe(a->passphrase);
  g_free(a->passphrase);
  g_free(a->path);
  a->passphrase = passphrase ? g_strdup(passphrase) : NULL;
  a->path = passphrase ? g_strdup(network_path) : NULL;
}

void tui_agent_free(TuiAgent *a)
{
  if (!a)
    return;

  if (a->registered && a->conn) {
    GVariant *r = g_dbus_connection_call_sync(a->conn, IWD_SERVICE, "/net/connman/iwd",
                                              "net.connman.iwd.AgentManager", "UnregisterAgent",
                                              g_variant_new("(o)", AGENT_PATH), NULL,
                                              G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (r)
      g_variant_unref(r);
  }

  if (a->reg_id)
    g_dbus_connection_unregister_object(a->conn, a->reg_id);
  if (a->node)
    g_dbus_node_info_unref(a->node);
  g_clear_object(&a->conn);
  tui_agent_set_passphrase(a, NULL, NULL);
  g_free(a->error);
  g_free(a);
}
