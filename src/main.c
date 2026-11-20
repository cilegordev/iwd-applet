#include "config.h"
#include <gtk/gtk.h>
#include "iwd-client.h"
#include "iwd-agent.h"
#include "tray.h"
#include "app-notify.h"

static struct {
  IwdClient *client;
  IwdAgent *agent;
  Tray *tray;
  IwdState prev_state;
  char *prev_name;
} app;

static void on_changed(IwdClient *c, gpointer data)
{
  IwdState st = iwd_client_get_state(c);
  const char *name = iwd_client_get_connected_name(c);

  iwd_agent_set_daemon_up(app.agent, st != IWD_STATE_NO_DAEMON);
  tray_update(app.tray);

  if (app.prev_state != IWD_STATE_NO_DAEMON) {
    if (st == IWD_STATE_CONNECTED &&
        (app.prev_state != IWD_STATE_CONNECTED || g_strcmp0(app.prev_name, name) != 0)) {
      char *body = g_strdup_printf("Connected to %s", name ? name : "?");
      app_notify_show("Wi-Fi", body, "network-wireless-symbolic");
      g_free(body);
    } else if (app.prev_state == IWD_STATE_CONNECTED && st != IWD_STATE_CONNECTED) {
      app_notify_show("Wi-Fi", "Disconnected", "network-wireless-offline-symbolic");
    }
  }

  g_free(app.prev_name);
  app.prev_name = g_strdup(name);
  app.prev_state = st;
}

static void on_error(IwdClient *c, const char *message, gpointer data)
{
  app_notify_show("Wi-Fi Error", message, "dialog-error");
}

static void on_startup(GApplication *a, gpointer data)
{
  g_application_hold(a); /* stay alive without any windows */
  app_notify_init();

  app.prev_state = IWD_STATE_NO_DAEMON;
  app.client = iwd_client_new(on_changed, on_error, NULL);
  app.agent = iwd_agent_new(app.client);
  app.tray = tray_new(app.client);
}

static void on_activate(GApplication *a, gpointer data)
{
}

static void on_shutdown(GApplication *a, gpointer data)
{
  tray_free(app.tray);
  iwd_agent_free(app.agent);
  iwd_client_free(app.client);
  g_free(app.prev_name);
  app_notify_shutdown();
}

int main(int argc, char **argv)
{
  GtkApplication *a = gtk_application_new("org.iwdapplet.IwdApplet", G_APPLICATION_DEFAULT_FLAGS);

  g_signal_connect(a, "startup", G_CALLBACK(on_startup), NULL);
  g_signal_connect(a, "activate", G_CALLBACK(on_activate), NULL);
  g_signal_connect(a, "shutdown", G_CALLBACK(on_shutdown), NULL);

  int rc = g_application_run(G_APPLICATION(a), argc, argv);
  g_object_unref(a);
  return rc;
}
