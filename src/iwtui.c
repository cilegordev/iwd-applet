/* iwtui: a small text-mode interface for iwd.
 *
 *   1. Connect to a Wi-Fi network (the password is asked for right there)
 *   2. Turn Wi-Fi on or off
 *   3. Exit
 *
 * It talks to iwd through the same D-Bus client as the tray applet, and registers its own iwd
 * agent so that it can hand the typed password to iwd. The screen uses a plain black
 * background. */
#define _XOPEN_SOURCE_EXTENDED 1
#include "config.h"

#include <glib.h>
#include <glib-unix.h>
#include <ncurses.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wctype.h>

#include "iwd-client.h"
#include "iwtui-agent.h"

#define MIN_COLS  44
#define MIN_LINES 12
#define MAX_PASSWORD_BYTES 120

enum { CP_NORMAL = 1, CP_SELECT, CP_OK, CP_ERROR, CP_HINT };

typedef enum {
  SCREEN_MAIN,
  SCREEN_NETWORKS,
  SCREEN_PASSWORD,
  SCREEN_BUSY,
  SCREEN_MESSAGE
} Screen;

typedef enum {
  MSG_INFO,
  MSG_OK,
  MSG_ERROR
} MsgKind;

typedef struct {
  GMainLoop *loop;
  IwdClient *client;
  TuiAgent *agent;

  Screen screen;
  gint main_sel;

  gchar *sel_path;   /* selected network, by object path (the list reorders itself) */
  gint net_top;      /* first visible row of the network list */

  gchar *target_path; /* network being connected */
  gchar *target_name;
  gboolean had_password;
  GString *password;
  gboolean show_password;

  Screen after_message;
  MsgKind msg_kind;
  gchar *msg_text;
} Ui;

static gboolean use_colors;

/* ---------- text helpers (UTF-8 aware) ---------- */

/* At most `cols` characters; a "~" marks a truncated string. */
static gchar *fit(const char *s, int cols)
{
  glong len = g_utf8_strlen(s, -1);

  if (cols <= 0)
    return g_strdup("");
  if (len <= cols)
    return g_strdup(s);

  const gchar *end = g_utf8_offset_to_pointer(s, cols - 1);
  gchar *head = g_strndup(s, end - s);
  gchar *res = g_strconcat(head, "~", NULL);
  g_free(head);
  return res;
}

/* Exactly `cols` characters: truncated or padded with spaces. */
static gchar *pad(const char *s, int cols)
{
  gchar *f = fit(s, cols);
  GString *out = g_string_new(f);

  for (glong n = g_utf8_strlen(f, -1); n < cols; n++)
    g_string_append_c(out, ' ');
  g_free(f);
  return g_string_free(out, FALSE);
}

static GPtrArray *wrap_text(const char *text, int width)
{
  GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
  gchar **words = g_strsplit_set(text, " \n", -1);
  GString *cur = g_string_new(NULL);

  for (gchar **w = words; *w; w++) {
    if (!**w)
      continue;
    if (cur->len && g_utf8_strlen(cur->str, -1) + 1 + g_utf8_strlen(*w, -1) > width) {
      g_ptr_array_add(lines, g_strdup(cur->str));
      g_string_truncate(cur, 0);
    }
    if (cur->len)
      g_string_append_c(cur, ' ');
    g_string_append(cur, *w);
  }
  if (cur->len)
    g_ptr_array_add(lines, g_strdup(cur->str));

  g_string_free(cur, TRUE);
  g_strfreev(words);
  return lines;
}

/* ---------- drawing ---------- */

static int attr_normal(void) { return use_colors ? COLOR_PAIR(CP_NORMAL) : A_NORMAL; }
static int attr_select(void) { return use_colors ? COLOR_PAIR(CP_SELECT) : A_REVERSE; }
static int attr_ok(void)     { return use_colors ? COLOR_PAIR(CP_OK) | A_BOLD : A_BOLD; }
static int attr_error(void)  { return use_colors ? COLOR_PAIR(CP_ERROR) | A_BOLD : A_BOLD; }
static int attr_hint(void)   { return use_colors ? COLOR_PAIR(CP_HINT) : A_DIM; }

static WINDOW *make_win(int h, int w, const char *title)
{
  WINDOW *win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);

  wbkgd(win, attr_normal());
  werase(win);
  wattron(win, attr_normal());
  box(win, 0, 0);
  if (title) {
    wattron(win, A_BOLD);
    mvwprintw(win, 0, 2, " %s ", title);
    wattroff(win, A_BOLD);
  }
  return win;
}

static void finish_win(WINDOW *win)
{
  wnoutrefresh(win);
  delwin(win);
}

static void put(WINDOW *win, int y, int x, int attr, const char *text, int width)
{
  gchar *f = fit(text, width);

  wattron(win, attr);
  mvwaddstr(win, y, x, f);
  wattroff(win, attr);
  g_free(f);
}

static void put_row(WINDOW *win, int y, int x, int width, int attr, const char *text)
{
  gchar *p = pad(text, width);

  wattron(win, attr);
  mvwaddstr(win, y, x, p);
  wattroff(win, attr);
  g_free(p);
}

static void draw_status(Ui *ui, WINDOW *win, int y, int x, int width)
{
  IwdClient *c = ui->client;
  gchar *text;
  int attr = attr_normal();

  switch (iwd_client_get_state(c)) {
  case IWD_STATE_NO_DAEMON:
    text = g_strdup("iwd is not running");
    attr = attr_error();
    break;
  case IWD_STATE_NO_DEVICE:
    text = g_strdup("No Wi-Fi device found");
    attr = attr_error();
    break;
  case IWD_STATE_OFF:
    text = g_strdup("Wi-Fi is off");
    break;
  case IWD_STATE_DISCONNECTED:
    text = g_strdup("Wi-Fi is on, not connected");
    break;
  case IWD_STATE_CONNECTING:
    text = g_strdup("Connecting...");
    break;
  case IWD_STATE_CONNECTED: {
    const char *name = iwd_client_get_connected_name(c);
    text = g_strdup_printf("Connected to %s (%d dBm)", name ? name : "?",
                           iwd_client_get_connected_signal(c));
    attr = attr_ok();
    break;
  }
  default:
    text = g_strdup("");
  }
  put(win, y, x, attr, text, width);
  g_free(text);
}

static void draw_main(Ui *ui)
{
  const int w = 48, h = 11;
  IwdState st = iwd_client_get_state(ui->client);
  WINDOW *win = make_win(h, w, "iwtui");

  draw_status(ui, win, 2, 3, w - 6);

  if (st != IWD_STATE_NO_DAEMON && !tui_agent_is_registered(ui->agent) && tui_agent_error(ui->agent)) {
    gchar *note = g_strdup_printf("Passwords unavailable: %s", tui_agent_error(ui->agent));
    put(win, 3, 3, attr_error(), note, w - 6);
    g_free(note);
  }

  const char *labels[3] = {
    "Connect to a Wi-Fi network",
    st == IWD_STATE_OFF ? "Turn Wi-Fi on" : "Turn Wi-Fi off",
    "Exit"
  };
  for (int i = 0; i < 3; i++)
    put_row(win, 5 + i, 3, w - 6, i == ui->main_sel ? attr_select() : attr_normal(), labels[i]);

  put(win, h - 2, 3, attr_hint(), "Up/Down: select   Enter: confirm   q: exit", w - 6);
  finish_win(win);
}

static int selected_index(Ui *ui, GPtrArray *nets)
{
  if (nets->len == 0)
    return -1;
  if (ui->sel_path) {
    for (guint i = 0; i < nets->len; i++) {
      IwdNetwork *n = g_ptr_array_index(nets, i);
      if (g_str_equal(n->path, ui->sel_path))
        return (int) i;
    }
  }
  return 0;
}

static void select_index(Ui *ui, GPtrArray *nets, int idx)
{
  if (nets->len == 0)
    return;
  idx = CLAMP(idx, 0, (int) nets->len - 1);
  IwdNetwork *n = g_ptr_array_index(nets, idx);
  g_free(ui->sel_path);
  ui->sel_path = g_strdup(n->path);
}

static void draw_networks(Ui *ui)
{
  GPtrArray *nets = iwd_client_get_networks(ui->client);
  int w = MIN(COLS - 2, 76);
  int h = MIN(LINES - 2, MAX((int) nets->len, 3) + 7);
  int list_h = h - 6;
  WINDOW *win = make_win(h, w, "Connect to a Wi-Fi network");
  int cols = w - 6;

  gchar *status = iwd_client_is_scanning(ui->client)
                    ? g_strdup("Scanning...")
                    : g_strdup_printf("%u network%s found", nets->len, nets->len == 1 ? "" : "s");
  put(win, 1, 3, attr_normal(), status, cols);
  g_free(status);

  int idx = selected_index(ui, nets);
  if (idx >= 0 && !ui->sel_path)
    select_index(ui, nets, idx);

  if (idx >= 0) {
    if (idx < ui->net_top)
      ui->net_top = idx;
    if (idx >= ui->net_top + list_h)
      ui->net_top = idx - list_h + 1;
  }
  ui->net_top = MAX(0, MIN(ui->net_top, (int) nets->len - list_h));

  if (nets->len == 0)
    put(win, 3, 3, attr_normal(), "No networks found. Press r to rescan.", cols);

  const int sig_w = 8, sec_w = 16;
  int ssid_w = MAX(cols - (2 + 1 + sig_w + 1 + sec_w), 6);

  for (int row = 0; row < list_h; row++) {
    int i = ui->net_top + row;
    if (i >= (int) nets->len)
      break;

    IwdNetwork *n = g_ptr_array_index(nets, i);
    gchar *ssid = pad(n->name ? n->name : "?", ssid_w);
    gchar *sig = g_strdup_printf("%d dBm", n->signal_dbm);
    gchar *sigp = pad(sig, sig_w);
    const char *kind = g_strcmp0(n->type, "open") == 0 ? "open"
                       : g_strcmp0(n->type, "8021x") == 0 ? "802.1x"
                       : "secured";
    gchar *sec = g_strdup_printf("%s%s", kind, n->known_path ? " (saved)" : "");
    gchar *secp = pad(sec, sec_w);
    gchar *line = g_strdup_printf("%s%s %s %s", n->connected ? "* " : "  ", ssid, sigp, secp);

    int attr = i == idx ? attr_select() : n->connected ? attr_ok() : attr_normal();
    put_row(win, 3 + row, 3, cols, attr, line);

    g_free(ssid); g_free(sig); g_free(sigp); g_free(sec); g_free(secp); g_free(line);
  }

  if (nets->len > (guint) list_h) {
    if (ui->net_top > 0)
      put(win, 2, w - 5, attr_hint(), "^", 1);
    if (ui->net_top + list_h < (int) nets->len)
      put(win, h - 4, w - 5, attr_hint(), "v", 1);
  }

  put(win, h - 2, 3, attr_hint(), "Enter: connect   r: rescan   Esc: back", cols);
  finish_win(win);
}

static void draw_password(Ui *ui)
{
  const int w = MIN(COLS - 2, 56), h = 9;
  int cols = w - 6;
  WINDOW *win = make_win(h, w, "Wi-Fi password");

  gchar *line = g_strdup_printf("Network: %s", ui->target_name ? ui->target_name : "?");
  put(win, 2, 3, attr_normal(), line, cols);
  g_free(line);

  put(win, 4, 3, attr_normal(), "Password:", 9);

  int field_w = cols - 10;
  GString *shown = g_string_new(NULL);
  glong n = g_utf8_strlen(ui->password->str, -1);
  if (ui->show_password) {
    g_string_assign(shown, ui->password->str);
  } else {
    for (glong i = 0; i < n; i++)
      g_string_append_c(shown, '*');
  }

  /* Show the end of long input, with a cursor mark. */
  glong len = g_utf8_strlen(shown->str, -1);
  const gchar *start = shown->str;
  if (len > field_w - 1)
    start = g_utf8_offset_to_pointer(shown->str, len - (field_w - 1));
  gchar *text = g_strconcat(start, "_", NULL);
  put_row(win, 4, 13, field_w, A_UNDERLINE | attr_normal(), text);
  g_free(text);
  g_string_free(shown, TRUE);

  put(win, h - 3, 3, attr_hint(), "Enter: connect   Tab: show/hide   Esc: cancel", cols);
  finish_win(win);
}

static void draw_busy(Ui *ui)
{
  const int w = MIN(COLS - 2, 50), h = 5;
  WINDOW *win = make_win(h, w, "Please wait");
  gchar *line = g_strdup_printf("Connecting to %s...", ui->target_name ? ui->target_name : "?");

  put(win, 2, 3, attr_normal(), line, w - 6);
  g_free(line);
  finish_win(win);
}

static void draw_message(Ui *ui)
{
  const int w = MIN(COLS - 2, 62);
  GPtrArray *lines = wrap_text(ui->msg_text ? ui->msg_text : "", w - 6);
  int n = MIN((int) lines->len, MAX(LINES - 8, 1));
  int h = n + 6;
  const char *title = ui->msg_kind == MSG_ERROR ? "Error" : ui->msg_kind == MSG_OK ? "Done" : "Info";
  int attr = ui->msg_kind == MSG_ERROR ? attr_error() : ui->msg_kind == MSG_OK ? attr_ok() : attr_normal();
  WINDOW *win = make_win(h, w, title);

  for (int i = 0; i < n; i++)
    put(win, 2 + i, 3, attr, g_ptr_array_index(lines, i), w - 6);
  put(win, h - 2, 3, attr_hint(), "Press Enter to continue", w - 6);

  g_ptr_array_unref(lines);
  finish_win(win);
}

static void redraw(Ui *ui)
{
  erase();
  wnoutrefresh(stdscr);

  if (LINES < MIN_LINES || COLS < MIN_COLS) {
    attron(attr_normal());
    mvaddstr(0, 0, "Terminal too small");
    attroff(attr_normal());
    wnoutrefresh(stdscr);
    doupdate();
    return;
  }

  switch (ui->screen) {
  case SCREEN_MAIN:     draw_main(ui); break;
  case SCREEN_NETWORKS: draw_networks(ui); break;
  case SCREEN_PASSWORD: draw_password(ui); break;
  case SCREEN_BUSY:     draw_busy(ui); break;
  case SCREEN_MESSAGE:  draw_message(ui); break;
  }
  doupdate();
}

/* ---------- actions ---------- */

static void wipe_password(Ui *ui)
{
  if (ui->password->len)
    explicit_bzero(ui->password->str, ui->password->len);
  g_string_truncate(ui->password, 0);
}

static void show_message(Ui *ui, MsgKind kind, Screen after, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  g_free(ui->msg_text);
  ui->msg_text = g_strdup_vprintf(fmt, ap);
  va_end(ap);

  ui->msg_kind = kind;
  ui->after_message = after;
  ui->screen = SCREEN_MESSAGE;
}

/* The list is only meaningful while Wi-Fi is on. */
static void sanitize_screen(Ui *ui)
{
  IwdState st = iwd_client_get_state(ui->client);

  if (ui->screen == SCREEN_NETWORKS && st < IWD_STATE_DISCONNECTED)
    ui->screen = SCREEN_MAIN;
  if (ui->screen == SCREEN_MESSAGE && ui->after_message == SCREEN_NETWORKS && st < IWD_STATE_DISCONNECTED)
    ui->after_message = SCREEN_MAIN;
}

static void on_connect_done(IwdClient *c, const char *error, gpointer data)
{
  Ui *ui = data;

  tui_agent_set_passphrase(ui->agent, NULL, NULL);
  if (error) {
    show_message(ui, MSG_ERROR, SCREEN_NETWORKS, "Could not connect to \"%s\": %s%s",
                 ui->target_name ? ui->target_name : "?", error,
                 ui->had_password ? " (check the password)" : "");
  } else {
    show_message(ui, MSG_OK, SCREEN_MAIN, "Connected to \"%s\".", ui->target_name ? ui->target_name : "?");
  }
  sanitize_screen(ui);
  redraw(ui);
}

static void start_connect(Ui *ui, const char *password)
{
  ui->had_password = password != NULL;
  tui_agent_set_passphrase(ui->agent, ui->target_path, password);
  ui->screen = SCREEN_BUSY;
  iwd_client_connect_async(ui->client, ui->target_path, on_connect_done, ui);
}

static void networks_connect_selected(Ui *ui)
{
  GPtrArray *nets = iwd_client_get_networks(ui->client);
  int idx = selected_index(ui, nets);

  if (idx < 0)
    return;

  IwdNetwork *n = g_ptr_array_index(nets, idx);
  gchar *name = g_strdup(n->name ? n->name : "?");
  gboolean secured = g_strcmp0(n->type, "open") != 0;

  g_free(ui->target_path);
  g_free(ui->target_name);
  ui->target_path = g_strdup(n->path);
  ui->target_name = g_strdup(name);

  if (n->connected) {
    show_message(ui, MSG_INFO, SCREEN_NETWORKS, "Already connected to \"%s\".", name);
  } else if (g_strcmp0(n->type, "8021x") == 0) {
    show_message(ui, MSG_ERROR, SCREEN_NETWORKS, "Enterprise (802.1x) networks are not supported yet.");
  } else if (secured && !n->known_path) {
    if (!tui_agent_is_registered(ui->agent)) {
      const char *why = tui_agent_error(ui->agent);
      show_message(ui, MSG_ERROR, SCREEN_NETWORKS,
                   "Cannot ask for a password here: the iwd agent is not registered%s%s%s. "
                   "Another program (for example a tray applet) may already provide one.",
                   why ? " (" : "", why ? why : "", why ? ")" : "");
    } else {
      wipe_password(ui);
      ui->show_password = FALSE;
      ui->screen = SCREEN_PASSWORD;
    }
  } else {
    /* Open or already saved: no password needed. */
    start_connect(ui, NULL);
  }
  g_free(name);
}

static void main_activate(Ui *ui)
{
  IwdState st = iwd_client_get_state(ui->client);

  switch (ui->main_sel) {
  case 0:
    if (st == IWD_STATE_NO_DAEMON) {
      show_message(ui, MSG_ERROR, SCREEN_MAIN, "iwd is not running.");
    } else if (st == IWD_STATE_NO_DEVICE) {
      show_message(ui, MSG_ERROR, SCREEN_MAIN, "No Wi-Fi device found.");
    } else if (st == IWD_STATE_OFF) {
      show_message(ui, MSG_INFO, SCREEN_MAIN, "Wi-Fi is off. Turn it on first.");
    } else {
      g_clear_pointer(&ui->sel_path, g_free);
      ui->net_top = 0;
      ui->screen = SCREEN_NETWORKS;
      iwd_client_scan(ui->client);
    }
    break;
  case 1:
    if (st == IWD_STATE_NO_DAEMON)
      show_message(ui, MSG_ERROR, SCREEN_MAIN, "iwd is not running.");
    else if (st == IWD_STATE_NO_DEVICE)
      show_message(ui, MSG_ERROR, SCREEN_MAIN, "No Wi-Fi device found.");
    else
      iwd_client_set_powered(ui->client, st == IWD_STATE_OFF);
    break;
  default:
    g_main_loop_quit(ui->loop);
  }
}

/* ---------- keyboard ---------- */

static gboolean key_is_enter(int rc, wint_t ch)
{
  return (rc == KEY_CODE_YES && ch == KEY_ENTER) || (rc == OK && (ch == '\n' || ch == '\r'));
}

static gboolean key_is_up(int rc, wint_t ch)
{
  return (rc == KEY_CODE_YES && ch == KEY_UP) || (rc == OK && ch == 'k');
}

static gboolean key_is_down(int rc, wint_t ch)
{
  return (rc == KEY_CODE_YES && ch == KEY_DOWN) || (rc == OK && ch == 'j');
}

static gboolean key_is_back(int rc, wint_t ch)
{
  return (rc == OK && (ch == 27 || ch == 'q')) || (rc == KEY_CODE_YES && ch == KEY_LEFT);
}

static void handle_key(Ui *ui, int rc, wint_t ch)
{
  if (rc == KEY_CODE_YES && ch == KEY_RESIZE)
    return;

  switch (ui->screen) {
  case SCREEN_MAIN:
    if (key_is_up(rc, ch))
      ui->main_sel = (ui->main_sel + 2) % 3;
    else if (key_is_down(rc, ch))
      ui->main_sel = (ui->main_sel + 1) % 3;
    else if (key_is_enter(rc, ch))
      main_activate(ui);
    else if (rc == OK && ch == 'q')
      g_main_loop_quit(ui->loop);
    break;

  case SCREEN_NETWORKS: {
    GPtrArray *nets = iwd_client_get_networks(ui->client);
    int idx = selected_index(ui, nets);

    if (key_is_up(rc, ch))
      select_index(ui, nets, idx - 1);
    else if (key_is_down(rc, ch))
      select_index(ui, nets, idx + 1);
    else if (rc == KEY_CODE_YES && ch == KEY_HOME)
      select_index(ui, nets, 0);
    else if (rc == KEY_CODE_YES && ch == KEY_END)
      select_index(ui, nets, (int) nets->len - 1);
    else if (rc == KEY_CODE_YES && ch == KEY_PPAGE)
      select_index(ui, nets, idx - 8);
    else if (rc == KEY_CODE_YES && ch == KEY_NPAGE)
      select_index(ui, nets, idx + 8);
    else if (rc == OK && ch == 'r')
      iwd_client_scan(ui->client);
    else if (key_is_enter(rc, ch))
      networks_connect_selected(ui);
    else if (key_is_back(rc, ch))
      ui->screen = SCREEN_MAIN;
    break;
  }

  case SCREEN_PASSWORD:
    if (key_is_enter(rc, ch)) {
      if (ui->password->len > 0) {
        start_connect(ui, ui->password->str);
        wipe_password(ui);
      }
    } else if (rc == OK && ch == 27) {
      wipe_password(ui);
      ui->screen = SCREEN_NETWORKS;
    } else if (rc == OK && ch == '\t') {
      ui->show_password = !ui->show_password;
    } else if ((rc == KEY_CODE_YES && ch == KEY_BACKSPACE) || (rc == OK && (ch == 127 || ch == 8))) {
      if (ui->password->len > 0) {
        const gchar *end = ui->password->str + ui->password->len;
        const gchar *prev = g_utf8_prev_char(end);
        explicit_bzero((gchar *) prev, end - prev);
        g_string_truncate(ui->password, prev - ui->password->str);
      }
    } else if (rc == OK && iswprint(ch) && ui->password->len < MAX_PASSWORD_BYTES) {
      gchar utf8[8];
      gint len = g_unichar_to_utf8((gunichar) ch, utf8);
      g_string_append_len(ui->password, utf8, len);
    }
    break;

  case SCREEN_BUSY:
    break; /* iwd is working; the result arrives through on_connect_done() */

  case SCREEN_MESSAGE:
    if (key_is_enter(rc, ch) || (rc == OK && (ch == 27 || ch == ' ')))
      ui->screen = ui->after_message;
    break;
  }
}

static void handle_input(Ui *ui)
{
  wint_t ch;
  int rc;

  while ((rc = get_wch(&ch)) != ERR)
    handle_key(ui, rc, ch);

  sanitize_screen(ui);
  redraw(ui);
}

static gboolean on_stdin(gint fd, GIOCondition cond, gpointer data)
{
  Ui *ui = data;

  if (cond & (G_IO_HUP | G_IO_ERR)) {
    g_main_loop_quit(ui->loop);
    return G_SOURCE_REMOVE;
  }
  handle_input(ui);
  return G_SOURCE_CONTINUE;
}

static gboolean on_signal(gpointer data)
{
  g_main_loop_quit(((Ui *) data)->loop);
  return G_SOURCE_REMOVE;
}

/* A terminal resize produces no input, so nothing would redraw the screen on its own. */
static gboolean on_winch(gpointer data)
{
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0)
    resizeterm(ws.ws_row, ws.ws_col);
  redraw(data);
  return G_SOURCE_CONTINUE;
}

/* ---------- iwd callbacks ---------- */

static void on_changed(IwdClient *c, gpointer data)
{
  Ui *ui = data;

  tui_agent_set_daemon_up(ui->agent, iwd_client_get_state(c) != IWD_STATE_NO_DAEMON);
  sanitize_screen(ui);
  redraw(ui);
}

static void on_error(IwdClient *c, const char *message, gpointer data)
{
  Ui *ui = data;

  show_message(ui, MSG_ERROR, ui->screen == SCREEN_MESSAGE ? ui->after_message : ui->screen, "%s", message);
  sanitize_screen(ui);
  redraw(ui);
}

static void on_agent_changed(gpointer data)
{
  redraw(data);
}

/* GLib warnings would be printed over the screen; keep them quiet unless asked for. */
static void quiet_log(const gchar *domain, GLogLevelFlags level, const gchar *message, gpointer data)
{
  if (g_getenv("IWTUI_DEBUG"))
    fprintf(stderr, "%s: %s\n", domain ? domain : "iwtui", message);
}

/* ---------- main ---------- */

int main(int argc, char **argv)
{
  setlocale(LC_ALL, "");

  for (int i = 1; i < argc; i++) {
    if (g_str_equal(argv[i], "--version") || g_str_equal(argv[i], "-V")) {
      printf("iwtui %s\n", APP_VERSION);
      return 0;
    }
    if (g_str_equal(argv[i], "--help") || g_str_equal(argv[i], "-h")) {
      printf("Usage: iwtui [--version] [--help]\n\n"
             "A text-mode interface for iwd: connect to a Wi-Fi network (with password entry),\n"
             "turn Wi-Fi on or off, and exit.\n\n"
             "Keys: Up/Down (or j/k) select, Enter confirm, Esc go back, q exit.\n");
      return 0;
    }
    fprintf(stderr, "iwtui: unknown option '%s' (try --help)\n", argv[i]);
    return 2;
  }

  if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    fprintf(stderr, "iwtui: it must be run in a terminal\n");
    return 1;
  }

  g_log_set_default_handler(quiet_log, NULL);

  Ui ui = { 0 };
  ui.loop = g_main_loop_new(NULL, FALSE);
  ui.password = g_string_new(NULL);
  ui.agent = tui_agent_new(on_agent_changed, &ui);
  ui.client = iwd_client_new(on_changed, on_error, &ui);

  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE);
  nodelay(stdscr, TRUE);
  curs_set(0);
  set_escdelay(25);

  use_colors = has_colors();
  if (use_colors) {
    start_color();
    /* A plain black background: white text, inverted for the selected row. */
    init_pair(CP_NORMAL, COLOR_WHITE, COLOR_BLACK);
    init_pair(CP_SELECT, COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_OK, COLOR_GREEN, COLOR_BLACK);
    init_pair(CP_ERROR, COLOR_RED, COLOR_BLACK);
    init_pair(CP_HINT, COLOR_CYAN, COLOR_BLACK);
  }
  bkgd(attr_normal() | ' ');

  g_unix_fd_add(STDIN_FILENO, G_IO_IN | G_IO_HUP | G_IO_ERR, on_stdin, &ui);
  g_unix_signal_add(SIGINT, on_signal, &ui);
  g_unix_signal_add(SIGTERM, on_signal, &ui);
  g_unix_signal_add(SIGHUP, on_signal, &ui);
  g_unix_signal_add(SIGWINCH, on_winch, &ui);

  redraw(&ui);
  g_main_loop_run(ui.loop);

  endwin();

  wipe_password(&ui);
  tui_agent_free(ui.agent);
  iwd_client_free(ui.client);
  g_string_free(ui.password, TRUE);
  g_free(ui.sel_path);
  g_free(ui.target_path);
  g_free(ui.target_name);
  g_free(ui.msg_text);
  g_main_loop_unref(ui.loop);
  return 0;
}
