#pragma once
#include <gio/gio.h>

/* A minimal iwd agent for the text-mode interface. It does not ask anything itself: the interface
 * collects the password first and hands it over with tui_agent_set_passphrase(); the agent then
 * answers iwd's request with it. */
typedef struct _TuiAgent TuiAgent;
typedef void (*TuiAgentChangedFunc)(gpointer user_data);

TuiAgent *tui_agent_new(TuiAgentChangedFunc changed, gpointer user_data);
void tui_agent_free(TuiAgent *a);

/* Call on every state change; registers the agent with iwd when the daemon is up. */
void tui_agent_set_daemon_up(TuiAgent *a, gboolean up);

gboolean tui_agent_is_registered(TuiAgent *a);
/* Why registration failed, or NULL. */
const char *tui_agent_error(TuiAgent *a);

/* Passphrase to hand to iwd for the given Network object path; NULL clears it. */
void tui_agent_set_passphrase(TuiAgent *a, const char *network_path, const char *passphrase);
