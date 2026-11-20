#include "config.h"
#include "menu.h"
#include "dialogs.h"


/* ---------- icons ---------- */

static const char *signal_level(int dbm)
{
  if (dbm >= -50) return "excellent";
  if (dbm >= -60) return "good";
  if (dbm >= -70) return "ok";
  return "weak";
}

/* Signal icon. Secured networks use the "-secure-symbolic" variant when the theme has it;
 * otherwise this falls back to the plain icon and sets *text_lock to TRUE so the caller
 * can add a text marker instead. */
static char *signal_icon(int dbm, gboolean secure, gboolean *text_lock)
{
  if (text_lock)
    *text_lock = FALSE;

  if (secure) {
    char *name = g_strdup_printf("network-wireless-signal-%s-secure-symbolic", signal_level(dbm));
    if (gtk_icon_theme_has_icon(gtk_icon_theme_get_default(), name))
      return name;
    g_free(name);
    if (text_lock)
      *text_lock = TRUE;
  }
  return g_strdup_printf("network-wireless-signal-%s-symbolic", signal_level(dbm));
}

static gboolean network_is_secure(const IwdNetwork *n)
{
  return g_strcmp0(n->type, "open") != 0;
}

char *menu_status_icon(IwdClient *c)
{
  switch (iwd_client_get_state(c)) {
  case IWD_STATE_OFF:
    return g_strdup("network-wireless-disabled-symbolic");
  case IWD_STATE_CONNECTING:
    return g_strdup("network-wireless-acquiring-symbolic");
  case IWD_STATE_CONNECTED:
    return signal_icon(iwd_client_get_connected_signal(c), FALSE, NULL);
  default:
    return g_strdup("network-wireless-offline-symbolic");
  }
}

char *menu_status_text(IwdClient *c)
{
  switch (iwd_client_get_state(c)) {
  case IWD_STATE_NO_DAEMON:    return g_strdup("iwd is not running");
  case IWD_STATE_NO_DEVICE:    return g_strdup("No Wi-Fi device found");
  case IWD_STATE_OFF:          return g_strdup("Wi-Fi is off");
  case IWD_STATE_DISCONNECTED: return g_strdup("Not connected");
  case IWD_STATE_CONNECTING:   return g_strdup("Connecting…");
  case IWD_STATE_CONNECTED: {
    const char *name = iwd_client_get_connected_name(c);
    return g_strdup_printf("Connected: %s (%d dBm)", name ? name : "?",
                           iwd_client_get_connected_signal(c));
  }
  }
  return g_strdup("");
}

/* ---------- About ---------- */

static void on_about_response(GtkDialog *dlg, gint response, gpointer data)
{
  gtk_widget_destroy(GTK_WIDGET(dlg));
}

static void add_dep(GString *s, const char *name, const char *version)
{
  if (version && *version)
    g_string_append_printf(s, "  %s %s\n", name, version);
}

static const char *backend_description(void)
{
  if (g_str_equal(TRAY_BACKEND, "sni"))
    return "StatusNotifierItem (direct GDBus)";
  if (g_str_equal(TRAY_BACKEND, "statusicon"))
    return "GtkStatusIcon (XEmbed)";
  return "libappindicator";
}

void menu_show_about(void)
{
  static GtkWidget *about = NULL;

  if (about) {
    gtk_window_present(GTK_WINDOW(about));
    return;
  }

  GString *s = g_string_new("Wi-Fi tray applet for iwd.\n\n"
                            "Built with:\n");
  add_dep(s, "GTK+", DEP_GTK);
  add_dep(s, "GLib/GIO", DEP_GLIB);
  add_dep(s, "libappindicator", DEP_APPINDICATOR);
  add_dep(s, "libdbusmenu-glib", DEP_DBUSMENU);
  add_dep(s, "libnotify", DEP_NOTIFY);
  add_dep(s, "Compiler", BUILD_COMPILER);
  add_dep(s, "Meson", BUILD_MESON);
  g_string_append_printf(s, "\nTray backend: %s", backend_description());

  about = gtk_about_dialog_new();
  g_object_add_weak_pointer(G_OBJECT(about), (gpointer *) &about);
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(about), "iwd-applet");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(about), APP_VERSION);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(about), s->str);
  gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(about), "network-wireless");
  g_signal_connect(about, "response", G_CALLBACK(on_about_response), NULL);
  g_string_free(s, TRUE);

  gtk_window_present(GTK_WINDOW(about));
}

/* ---------- menu callbacks ---------- */

static void on_quit(GtkMenuItem *i, gpointer d)
{
  g_application_quit(g_application_get_default());
}

static void on_about(GtkMenuItem *i, gpointer d)
{
  menu_show_about();
}

static void on_scan(GtkMenuItem *i, gpointer d)
{
  iwd_client_scan(d);
}

/* Mouse clicks on "Rescan" are handled here and swallowed (return TRUE) so that the click
 * does not bubble up to the menu shell, which would close the menu. Keyboard activation still
 * goes through "activate" (on_scan) and closes the menu as usual. */
static gboolean on_scan_release(GtkWidget *w, GdkEventButton *ev, gpointer d)
{
  if (ev->button == GDK_BUTTON_PRIMARY)
    iwd_client_scan(d);
  return TRUE;
}

static void on_disconnect(GtkMenuItem *i, gpointer d)
{
  iwd_client_disconnect(d);
}

static void on_power_toggled(GtkCheckMenuItem *i, gpointer d)
{
  iwd_client_set_powered(d, gtk_check_menu_item_get_active(i));
}

static void on_network(GtkMenuItem *i, gpointer d)
{
  const char *path = g_object_get_data(G_OBJECT(i), "path");
  if (path)
    iwd_client_connect(d, path);
}

static void on_forget(GtkMenuItem *i, gpointer d)
{
  const char *path = g_object_get_data(G_OBJECT(i), "path");
  if (path)
    iwd_client_forget(d, path);
}

static void on_hidden_ssid(const char *text, gpointer d)
{
  if (text)
    iwd_client_connect_hidden(d, text);
}

static void on_hidden(GtkMenuItem *i, gpointer d)
{
  dialogs_ask_text("Hidden Network", "Enter the network name (SSID):", FALSE, on_hidden_ssid, d);
}

/* ---------- menu builders ---------- */

static GtkWidget *append_item(GtkWidget *menu, const char *label, GCallback cb, gpointer data)
{
  GtkWidget *it = gtk_menu_item_new_with_label(label); /* literal label, no mnemonics */
  if (cb)
    g_signal_connect(it, "activate", cb, data);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
  return it;
}

static GtkWidget *append_icon_item(GtkWidget *menu, const char *label, const char *icon,
                                   GCallback cb, gpointer data)
{
  GtkWidget *it = gtk_image_menu_item_new_with_label(label);
  GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
  gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(it), img);
  gtk_image_menu_item_set_always_show_image(GTK_IMAGE_MENU_ITEM(it), TRUE);
  if (cb)
    g_signal_connect(it, "activate", cb, data);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
  return it;
}

static void append_sep(GtkWidget *menu)
{
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

static void clear_menu(GtkWidget *menu)
{
  GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
  g_list_free_full(kids, (GDestroyNotify) gtk_widget_destroy);
}

static void append_app_items(GtkWidget *menu)
{
  append_icon_item(menu, "About", "help-about-symbolic", G_CALLBACK(on_about), NULL);
  append_icon_item(menu, "Quit", "application-exit-symbolic", G_CALLBACK(on_quit), NULL);
}

void menu_build_context(GtkWidget *menu)
{
  clear_menu(menu);
  append_app_items(menu);
  gtk_widget_show_all(menu);
}

/* Everything that influences what the main menu shows, flattened into one string. Used to skip
 * rebuilding a menu (which would make an open menu flicker) when nothing visible changed. */
static char *menu_signature(IwdClient *c, gboolean with_app_items)
{
  GString *s = g_string_new(NULL);
  char *status = menu_status_text(c);
  GPtrArray *nets = iwd_client_get_networks(c);

  g_string_append_printf(s, "%d|%s|%d|%d|", iwd_client_get_state(c), status,
                         iwd_client_is_scanning(c), with_app_items);
  g_free(status);

  for (guint i = 0; i < nets->len; i++) {
    IwdNetwork *n = g_ptr_array_index(nets, i);
    g_string_append_printf(s, "%s|%s|%s|%s|%d|%d;", n->path, n->name ? n->name : "",
                           n->type ? n->type : "", n->known_path ? n->known_path : "",
                           n->signal_dbm, n->connected);
  }
  return g_string_free(s, FALSE);
}

/* The size the menu wants, computed from its children and their grid positions. GTK's own
 * preferred size cannot be used here: while a menu is open it is capped to the space below the
 * position where the menu was popped up, so it never reports growth. */
/* Grid cell of a child. Items added with gtk_menu_shell_append() have no explicit attach values
 * (GTK places them one per row, in order), so fall back to their position in the child list. */
static void child_cell(GtkWidget *menu, GtkWidget *child, gint index,
                       gint *left, gint *right, gint *top, gint *bottom)
{
  gint l = -1, r = -1, t = -1, b = -1;

  gtk_container_child_get(GTK_CONTAINER(menu), child, "left-attach", &l, "right-attach", &r,
                          "top-attach", &t, "bottom-attach", &b, NULL);
  if (l < 0 || r <= l || t < 0 || b <= t) {
    l = 0;
    r = 1;
    t = index;
    b = index + 1;
  }
  *left = l;
  *right = r;
  *top = t;
  *bottom = b;
}

static void menu_natural_size(GtkWidget *menu, gint *width, gint *height)
{
  GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
  gint ncols = 1, nrows = 0, i = 0;

  for (GList *l = kids; l; l = l->next, i++) {
    gint left, right, top, bottom;
    child_cell(menu, l->data, i, &left, &right, &top, &bottom);
    ncols = MAX(ncols, right);
    nrows = MAX(nrows, bottom);
  }

  gint *col_w = g_new0(gint, ncols);
  gint *row_h = g_new0(gint, MAX(nrows, 1));
  gint span_w = 0;

  i = 0;
  for (GList *l = kids; l; l = l->next, i++) {
    GtkWidget *w = l->data;
    gint left, right, top, bottom, nat_w = 0, nat_h = 0;

    if (!gtk_widget_get_visible(w))
      continue;
    child_cell(menu, w, i, &left, &right, &top, &bottom);
    gtk_widget_get_preferred_width(w, NULL, &nat_w);
    gtk_widget_get_preferred_height(w, NULL, &nat_h);

    if (right - left == 1)
      col_w[left] = MAX(col_w[left], nat_w);
    else
      span_w = MAX(span_w, nat_w);
    if (bottom - top == 1)
      row_h[top] = MAX(row_h[top], nat_h);
  }

  gint w = 0, h = 0;
  for (gint c = 0; c < ncols; c++)
    w += col_w[c];
  for (gint r = 0; r < nrows; r++)
    h += row_h[r];
  w = MAX(w, span_w);

  GtkBorder pad = { 0 };
  GtkStyleContext *ctx = gtk_widget_get_style_context(menu);
  gtk_style_context_get_padding(ctx, gtk_style_context_get_state(ctx), &pad);

  *width = w + pad.left + pad.right;
  *height = h + pad.top + pad.bottom;

  g_free(col_w);
  g_free(row_h);
  g_list_free(kids);
}

/* Height that a menu may use: the workarea of the monitor under the pointer (panels excluded),
 * minus a margin for the menu's own border and padding. */
static gint available_menu_height(void)
{
  GdkDisplay *display = gdk_display_get_default();
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(display));
  GdkRectangle wa = { 0 };
  gint x = 0, y = 0;

  gdk_device_get_position(pointer, NULL, &x, &y);
  GdkMonitor *mon = gdk_display_get_monitor_at_point(display, x, y);
  if (mon) {
    gdk_monitor_get_workarea(mon, &wa);
    if (wa.height <= 0)
      gdk_monitor_get_geometry(mon, &wa);
  }
  return wa.height > 0 ? wa.height - 24 : 600;
}

/* If the menu in its single-column form is taller than the screen allows, spread the network rows
 * over as many columns as needed. Everything above and below the networks (switch, status line,
 * actions) spans all columns. This avoids GTK's scrolling menu, so every network stays visible. */
static void arrange_columns(GtkWidget *menu)
{
  GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
  GtkRequisition req;
  gint n = 0, top = 0;
  GtkWidget *first_net = NULL;

  for (GList *l = kids; l; l = l->next) {
    if (g_object_get_data(G_OBJECT(l->data), "iwd-net")) {
      if (!first_net)
        first_net = l->data;
      n++;
    } else if (!first_net) {
      top++;
    }
  }
  if (n == 0) {
    g_list_free(kids);
    return;
  }

  menu_natural_size(menu, &req.width, &req.height);
  gint avail = available_menu_height();
  if (req.height <= avail) {
    g_list_free(kids);
    return;
  }

  gint row_h = 0;
  gtk_widget_get_preferred_height(first_net, NULL, &row_h);
  if (row_h <= 0)
    row_h = 28;

  gint fixed = req.height - n * row_h;
  gint per_col = (avail - fixed) / row_h;
  if (per_col < 4)
    per_col = 4;
  gint cols = (n + per_col - 1) / per_col;
  per_col = (n + cols - 1) / cols; /* balance the columns */

  gint idx_net = 0, idx_top = 0, idx_bottom = 0;
  for (GList *l = kids; l; l = l->next) {
    GtkWidget *w = l->data;
    gint left, right, row;

    if (g_object_get_data(G_OBJECT(w), "iwd-net")) {
      left = idx_net / per_col;
      right = left + 1;
      row = top + idx_net % per_col;
      idx_net++;
    } else if (idx_net == 0) {
      left = 0;
      right = cols;
      row = idx_top++;
    } else {
      left = 0;
      right = cols;
      row = top + per_col + idx_bottom++;
    }
    gtk_container_child_set(GTK_CONTAINER(menu), w,
                            "left-attach", left, "right-attach", right,
                            "top-attach", row, "bottom-attach", row + 1, NULL);
  }
  g_list_free(kids);
}

gboolean menu_build_main(GtkWidget *menu, IwdClient *c, gboolean with_app_items, gboolean allow_columns)
{
  char *sig = menu_signature(c, with_app_items);
  GList *kids = gtk_container_get_children(GTK_CONTAINER(menu));
  gboolean had_children = kids != NULL;
  gboolean unchanged = had_children &&
                       g_strcmp0(sig, g_object_get_data(G_OBJECT(menu), "iwd-menu-sig")) == 0;
  g_list_free(kids);
  if (unchanged) {
    g_free(sig);
    return FALSE;
  }

  gint old_w = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu), "iwd-want-w"));
  gint old_h = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu), "iwd-want-h"));

  clear_menu(menu);

  IwdState st = iwd_client_get_state(c);
  GPtrArray *nets = iwd_client_get_networks(c);

  /* Wi-Fi switch comes first. */
  if (st >= IWD_STATE_OFF) {
    GtkWidget *pw = gtk_check_menu_item_new_with_label("Enable Wi-Fi");
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(pw), st != IWD_STATE_OFF);
    g_signal_connect(pw, "toggled", G_CALLBACK(on_power_toggled), c);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pw);
  }

  /* Status line with the same icon as the tray. The connected network is only shown here,
   * not in the list below. */
  char *status = menu_status_text(c);
  char *status_icon = menu_status_icon(c);
  gtk_widget_set_sensitive(append_icon_item(menu, status, status_icon, NULL, NULL), FALSE);
  g_free(status);
  g_free(status_icon);

  if (st >= IWD_STATE_DISCONNECTED) {
    append_sep(menu);

    guint shown = 0;
    for (guint i = 0; i < nets->len; i++) {
      IwdNetwork *n = g_ptr_array_index(nets, i);
      if (n->connected)
        continue;

      gboolean text_lock = FALSE;
      char *icon = signal_icon(n->signal_dbm, network_is_secure(n), &text_lock);
      char *label = g_strdup_printf("%s  (%d dBm)%s", n->name ? n->name : "?", n->signal_dbm,
                                    text_lock ? "  [secured]" : "");
      GtkWidget *it = append_icon_item(menu, label, icon, G_CALLBACK(on_network), c);
      g_object_set_data(G_OBJECT(it), "iwd-net", GINT_TO_POINTER(1));
      g_object_set_data_full(G_OBJECT(it), "path", g_strdup(n->path), g_free);
      g_free(label);
      g_free(icon);
      shown++;
    }
    if (shown == 0)
      gtk_widget_set_sensitive(append_item(menu,
          iwd_client_get_connected_name(c) ? "No other networks found" : "No networks found",
          NULL, NULL), FALSE);

    append_sep(menu);
    if (iwd_client_is_scanning(c)) {
      gtk_widget_set_sensitive(append_item(menu, "Scanning…", NULL, NULL), FALSE);
    } else {
      GtkWidget *rescan = append_item(menu, "Rescan", G_CALLBACK(on_scan), c);
      g_signal_connect(rescan, "button-release-event", G_CALLBACK(on_scan_release), c);
    }
    if (st == IWD_STATE_CONNECTED || st == IWD_STATE_CONNECTING)
      append_item(menu, "Disconnect", G_CALLBACK(on_disconnect), c);
    append_item(menu, "Connect to Hidden Network…", G_CALLBACK(on_hidden), c);

    GtkWidget *sub = gtk_menu_new();
    guint nk = 0;
    for (guint i = 0; i < nets->len; i++) {
      IwdNetwork *n = g_ptr_array_index(nets, i);
      if (!n->known_path)
        continue;
      GtkWidget *it = append_item(sub, n->name ? n->name : "?", G_CALLBACK(on_forget), c);
      g_object_set_data_full(G_OBJECT(it), "path", g_strdup(n->known_path), g_free);
      nk++;
    }
    if (nk) {
      GtkWidget *forget = gtk_menu_item_new_with_label("Forget Network");
      gtk_menu_item_set_submenu(GTK_MENU_ITEM(forget), sub);
      gtk_menu_shell_append(GTK_MENU_SHELL(menu), forget);
    } else {
      gtk_widget_destroy(sub);
    }
  }

  if (with_app_items) {
    append_sep(menu);
    append_app_items(menu);
  }
  gtk_widget_show_all(menu);
  if (allow_columns)
    arrange_columns(menu);
  g_object_set_data_full(G_OBJECT(menu), "iwd-menu-sig", sig, g_free);

  gint new_w = 0, new_h = 0;
  menu_natural_size(menu, &new_w, &new_h);
  g_object_set_data(G_OBJECT(menu), "iwd-want-w", GINT_TO_POINTER(new_w));
  g_object_set_data(G_OBJECT(menu), "iwd-want-h", GINT_TO_POINTER(new_h));
  return new_w != old_w || new_h != old_h;
}

/* Where the menu was requested. Kept on the menu so that gtk_menu_reposition() can recompute the
 * placement later (e.g. after the list grew) without looking at the current pointer position. */
typedef struct {
  gint x;
  gint y;
} PopupPos;

/* Positioning rules:
 *  - The menu's left edge lines up with the click; if it does not fit on the right side of the
 *    screen it is slid to the left (not flipped), so it looks like a native panel menu.
 *  - If the click happened on a panel (outside the workarea), attach the menu to the panel edge:
 *    a bottom panel makes the menu grow upward, a top panel makes it grow downward.
 *  - Elsewhere the menu opens below the pointer, or above it if there is no room below. */
static void popup_position(GtkMenu *menu, gint *x, gint *y, gboolean *push_in, gpointer data)
{
  PopupPos *pp = g_object_get_data(G_OBJECT(menu), "iwd-popup-pos");
  GtkRequisition req;
  GdkRectangle wa = { 0 };

  req.width = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu), "iwd-want-w"));
  req.height = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menu), "iwd-want-h"));
  if (req.width <= 0 || req.height <= 0) {
    gtk_widget_get_preferred_size(GTK_WIDGET(menu), NULL, &req);
  } else {
    /* The width is not capped by GTK (only the height is), and it includes the check/arrow
     * columns that the child measurement above does not know about. */
    gint gtk_w = 0;
    gtk_widget_get_preferred_width(GTK_WIDGET(menu), NULL, &gtk_w);
    req.width = MAX(req.width, gtk_w);
  }

  GdkMonitor *mon = gdk_display_get_monitor_at_point(gdk_display_get_default(), pp->x, pp->y);
  if (mon) {
    gdk_monitor_get_workarea(mon, &wa);
    if (wa.width <= 0)
      gdk_monitor_get_geometry(mon, &wa);
  }

  gint mx = pp->x;
  gint my = pp->y;

  if (wa.width > 0) {
    if (mx + req.width > wa.x + wa.width)
      mx = wa.x + wa.width - req.width;
    if (mx < wa.x)
      mx = wa.x;

    if (pp->y >= wa.y + wa.height) {
      my = wa.y + wa.height - req.height;
    } else if (pp->y < wa.y) {
      my = wa.y;
    } else if (pp->y + req.height > wa.y + wa.height) {
      my = pp->y - req.height >= wa.y ? pp->y - req.height : wa.y + wa.height - req.height;
    }
    if (my < wa.y)
      my = wa.y;
  }

  *x = mx;
  *y = my;
  *push_in = TRUE; /* GTK keeps the menu on screen and scrolls it if it is taller than the screen */
}

void menu_popup_at(GtkWidget *menu, gint x, gint y)
{
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
  PopupPos *pp = g_new0(PopupPos, 1);

  pp->x = x;
  pp->y = y;
  g_object_set_data_full(G_OBJECT(menu), "iwd-popup-pos", pp, g_free);

  /* D-Bus method calls arrive without a GDK event, so use the device-based popup API. */
  gtk_menu_popup_for_device(GTK_MENU(menu), pointer, NULL, NULL, popup_position, NULL, NULL,
                            0, gtk_get_current_event_time());
}

void menu_popup_at_pointer(GtkWidget *menu)
{
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
  gint x = 0, y = 0;

  gdk_device_get_position(pointer, NULL, &x, &y);
  menu_popup_at(menu, x, y);
}

static gboolean reposition_idle(gpointer data)
{
  GtkWidget *menu = data;

  if (gtk_widget_get_visible(menu))
    gtk_menu_reposition(GTK_MENU(menu));
  g_object_unref(menu);
  return G_SOURCE_REMOVE;
}

void menu_reposition(GtkWidget *menu)
{
  if (!gtk_widget_get_visible(menu))
    return;

  gtk_menu_reposition(GTK_MENU(menu));
  gtk_widget_queue_resize(menu);
  /* GTK clamps with the menu's previous allocation; once the new size has been allocated, place
   * it once more so the final position is exact. */
  g_idle_add_full(G_PRIORITY_LOW, reposition_idle, g_object_ref(menu), NULL);
}
