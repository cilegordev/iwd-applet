#pragma once
#include <gtk/gtk.h>

/* text == NULL means the dialog was canceled. Called exactly once. */
typedef void (*DialogTextFunc)(const char *text, gpointer user_data);

/* Non-modal dialog with a single text entry. Returns the dialog widget so it can be
 * canceled from outside with gtk_dialog_response(..., GTK_RESPONSE_CANCEL). */
GtkWidget *dialogs_ask_text(const char *title, const char *message, gboolean secret,
                            DialogTextFunc func, gpointer user_data);
