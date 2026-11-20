#include "dialogs.h"

typedef struct {
  GtkWidget *entry;
  DialogTextFunc func;
  gpointer data;
} AskCtx;

static void on_response(GtkDialog *dlg, gint response, gpointer user_data)
{
  AskCtx *ctx = user_data;
  gchar *text = NULL;

  if (response == GTK_RESPONSE_OK)
    text = g_strdup(gtk_entry_get_text(GTK_ENTRY(ctx->entry)));

  gtk_widget_destroy(GTK_WIDGET(dlg));
  ctx->func(text, ctx->data);

  g_free(text);
  g_free(ctx);
}

static void on_entry_changed(GtkEditable *e, gpointer dlg)
{
  gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), GTK_RESPONSE_OK,
                                    gtk_entry_get_text_length(GTK_ENTRY(e)) > 0);
}

static void on_show_toggled(GtkToggleButton *b, gpointer entry)
{
  gtk_entry_set_visibility(GTK_ENTRY(entry), gtk_toggle_button_get_active(b));
}

GtkWidget *dialogs_ask_text(const char *title, const char *message, gboolean secret,
                            DialogTextFunc func, gpointer user_data)
{
  GtkWidget *dlg = gtk_dialog_new_with_buttons(title, NULL, 0,
                                               "_Cancel", GTK_RESPONSE_CANCEL,
                                               "_Connect", GTK_RESPONSE_OK,
                                               NULL);
  gtk_window_set_icon_name(GTK_WINDOW(dlg), "network-wireless");
  gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
  gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
  gtk_dialog_set_response_sensitive(GTK_DIALOG(dlg), GTK_RESPONSE_OK, FALSE);

  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_container_set_border_width(GTK_CONTAINER(box), 12);
  gtk_container_add(GTK_CONTAINER(area), box);

  GtkWidget *label = gtk_label_new(message);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);

  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_entry_set_width_chars(GTK_ENTRY(entry), 32);
  gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
  g_signal_connect(entry, "changed", G_CALLBACK(on_entry_changed), dlg);

  if (secret) {
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    GtkWidget *check = gtk_check_button_new_with_label("Show password");
    g_signal_connect(check, "toggled", G_CALLBACK(on_show_toggled), entry);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
  }

  AskCtx *ctx = g_new0(AskCtx, 1);
  ctx->entry = entry;
  ctx->func = func;
  ctx->data = user_data;
  g_signal_connect(dlg, "response", G_CALLBACK(on_response), ctx);

  gtk_widget_show_all(dlg);
  gtk_window_present(GTK_WINDOW(dlg));
  return dlg;
}
