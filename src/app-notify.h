#pragma once

void app_notify_init(void);
void app_notify_shutdown(void);
void app_notify_show(const char *summary, const char *body, const char *icon);
