#pragma once
#include "iwd-client.h"

typedef struct _Tray Tray;

Tray *tray_new(IwdClient *client);
void  tray_free(Tray *t);
void  tray_update(Tray *t);
