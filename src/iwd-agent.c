#include "iwd-agent.h"
#include "dialogs.h"

#define IWD_SERVICE  "net.connman.iwd"

/* Can be overridden at build time to test against a mock iwd on the session bus. */
#ifndef IWD_BUS_TYPE
#define IWD_BUS_TYPE G_BUS_TYPE_SYSTEM
#endif
#define AGENT_PATH   "/org/iwdapplet/Agent"
#define ERR_CANCELED "net.connman.iwd.Agent.Error.Canceled"

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

struct _IwdAgent {
  IwdClient *client;
  GDBusConnection *conn;
  GDBusNodeInfo *node;
  guint reg_id;
  gboolean registered;
  gboolean registering;

  GtkWidget *dialog;
  GDBusMethodInvocation *pending;
};

static void on_text(const char *text, gpointer user_data)
{
  IwdAgent *a = user_data;
  GDBusMethodInvocation *inv = a->pending;

  a->pending = NULL;
  a->dialog = NULL;
  if (!inv)
    return;

  if (text)
    g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", text));
  else
    g_dbus_method_invocation_return_dbus_error(inv, ERR_CANCELED, "Canceled by the user");
}

static void handle_method(GDBusConnection *conn, const gchar *sender, const gchar *object_path,
                          const gchar *interface_name, const gchar *method, GVariant *params,
                          GDBusMethodInvocation *inv, gpointer user_data)
{
  IwdAgent *a = user_data;

  if (g_str_equal(method, "Release")) {
    a->registered = FALSE;
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (g_str_equal(method, "Cancel")) {
    if (a->dialog)
      gtk_dialog_response(GTK_DIALOG(a->dialog), GTK_RESPONSE_CANCEL);
    g_dbus_method_invocation_return_value(inv, NULL);
  } else if (g_str_equal(method, "RequestPassphrase") ||
             g_str_equal(method, "RequestPrivateKeyPassphrase")) {
    const gchar *path = NULL;
    g_variant_get(params, "(&o)", &path);

    if (a->pending) {
      g_dbus_method_invocation_return_dbus_error(inv, ERR_CANCELED, "Another request is already in progress");
      return;
    }

    gchar *name = iwd_client_network_name(a->client, path);
    gchar *msg = g_strdup_printf("Enter the password for “%s”:", name ? name : path);
    a->pending = inv;
    a->dialog = dialogs_ask_text("Wi-Fi Password", msg, TRUE, on_text, a);
    g_free(msg);
    g_free(name);
  } else {
    /* 802.1x (username/password) is not supported yet */
    g_dbus_method_invocation_return_dbus_error(inv, ERR_CANCELED, "Not supported yet");
  }
}

static const GDBusInterfaceVTable vtable = { handle_method, NULL, NULL, { 0 } };

IwdAgent *iwd_agent_new(IwdClient *client)
{
  GError *err = NULL;
  IwdAgent *a = g_new0(IwdAgent, 1);
  a->client = client;

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
  g_warning("iwd agent disabled: %s", err ? err->message : "?");
  g_clear_error(&err);
  iwd_agent_free(a);
  return NULL;
}

static void on_registered(GObject *src, GAsyncResult *res, gpointer user_data)
{
  IwdAgent *a = user_data;
  GError *err = NULL;
  GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);

  a->registering = FALSE;
  if (r) {
    g_variant_unref(r);
    a->registered = TRUE;
    return;
  }

  gchar *remote = g_dbus_error_get_remote_error(err);
  if (remote && g_str_equal(remote, "net.connman.iwd.AlreadyExists"))
    a->registered = TRUE;
  else
    g_warning("RegisterAgent failed: %s", err->message);
  g_free(remote);
  g_error_free(err);
}

void iwd_agent_set_daemon_up(IwdAgent *a, gboolean up)
{
  if (!a)
    return;

  if (!up) {
    a->registered = FALSE;
    a->registering = FALSE;
    return;
  }

  if (a->registered || a->registering)
    return;

  a->registering = TRUE;
  g_dbus_connection_call(a->conn, IWD_SERVICE, "/net/connman/iwd",
                         "net.connman.iwd.AgentManager", "RegisterAgent",
                         g_variant_new("(o)", AGENT_PATH), NULL,
                         G_DBUS_CALL_FLAGS_NONE, -1, NULL, on_registered, a);
}

void iwd_agent_free(IwdAgent *a)
{
  if (!a)
    return;

  if (a->dialog)
    gtk_dialog_response(GTK_DIALOG(a->dialog), GTK_RESPONSE_CANCEL);

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
  g_free(a);
}
