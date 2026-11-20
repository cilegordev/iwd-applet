#include "config.h"
#include "app-notify.h"
#include <glib.h>

#ifdef HAVE_LIBNOTIFY
#include <libnotify/notify.h>
#endif

void app_notify_init(void)
{
#ifdef HAVE_LIBNOTIFY
  notify_init("iwd-applet");
#endif
}

void app_notify_shutdown(void)
{
#ifdef HAVE_LIBNOTIFY
  notify_uninit();
#endif
}

void app_notify_show(const char *summary, const char *body, const char *icon)
{
#ifdef HAVE_LIBNOTIFY
  NotifyNotification *n = notify_notification_new(summary, body, icon);
  GError *err = NULL;

  notify_notification_set_timeout(n, 4000);
  if (!notify_notification_show(n, &err)) {
    g_warning("Notification failed: %s", err->message);
    g_error_free(err);
  }
  g_object_unref(n);
#else
  g_message("%s: %s", summary, body);
#endif
}
