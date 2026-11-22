#pragma once
#include <gio/gio.h>

/* Enum order matters: states are compared with >= in menu.c */
typedef enum {
  IWD_STATE_NO_DAEMON,
  IWD_STATE_NO_DEVICE,
  IWD_STATE_OFF,
  IWD_STATE_DISCONNECTED,
  IWD_STATE_CONNECTING,
  IWD_STATE_CONNECTED
} IwdState;

typedef struct {
  char *path;        /* object path Network */
  char *name;        /* SSID */
  char *type;        /* "open", "psk", "8021x" */
  char *known_path;  /* KnownNetwork object path, NULL if the network is not saved */
  int signal_dbm;
  gboolean connected;
} IwdNetwork;

typedef struct _IwdClient IwdClient;
typedef void (*IwdClientChangedFunc)(IwdClient *client, gpointer user_data);
typedef void (*IwdClientErrorFunc)(IwdClient *client, const char *message, gpointer user_data);

/* error is NULL when the operation succeeded, otherwise iwd's error message. */
typedef void (*IwdDoneFunc)(IwdClient *client, const char *error, gpointer user_data);
typedef IwdDoneFunc IwdConnectDoneFunc;

IwdClient  *iwd_client_new(IwdClientChangedFunc changed, IwdClientErrorFunc error, gpointer user_data);
void        iwd_client_free(IwdClient *c);

IwdState    iwd_client_get_state(IwdClient *c);
GPtrArray  *iwd_client_get_networks(IwdClient *c);        /* array of IwdNetwork*, owned by the client */
const char *iwd_client_get_connected_name(IwdClient *c);  /* NULL if not connected */
int         iwd_client_get_connected_signal(IwdClient *c);
gboolean    iwd_client_is_scanning(IwdClient *c);
char       *iwd_client_network_name(IwdClient *c, const char *network_path); /* g_free() */

void iwd_client_scan(IwdClient *c);
void iwd_client_connect(IwdClient *c, const char *network_path);
/* Like iwd_client_connect(), but reports the result to done(). */
void iwd_client_connect_async(IwdClient *c, const char *network_path,
                              IwdConnectDoneFunc done, gpointer user_data);
void iwd_client_connect_hidden(IwdClient *c, const char *ssid);
void iwd_client_disconnect(IwdClient *c);
void iwd_client_set_powered(IwdClient *c, gboolean powered);
void iwd_client_forget(IwdClient *c, const char *known_path);
/* Like iwd_client_forget(), but reports the result to done(). */
void iwd_client_forget_async(IwdClient *c, const char *known_path,
                             IwdDoneFunc done, gpointer user_data);

/* A saved network. Unlike IwdNetwork, it does not have to be in range. */
typedef struct {
  char *path;  /* object path KnownNetwork */
  char *name;  /* SSID */
  char *type;  /* "open", "psk", "8021x" */
} IwdKnownNetwork;

/* Newly allocated list of all saved networks, sorted by name. Free with g_ptr_array_unref(). */
GPtrArray *iwd_client_list_known_networks(IwdClient *c);
