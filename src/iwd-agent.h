#pragma once
#include "iwd-client.h"

typedef struct _IwdAgent IwdAgent;

IwdAgent *iwd_agent_new(IwdClient *client);
void iwd_agent_free(IwdAgent *a);

/* Call on every state change; registers or drops the agent with iwd depending on whether
 * the daemon is up. */
void iwd_agent_set_daemon_up(IwdAgent *a, gboolean up);
