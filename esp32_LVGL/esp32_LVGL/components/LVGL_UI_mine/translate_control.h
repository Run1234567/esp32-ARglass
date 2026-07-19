#ifndef _TRANSLATE_CONTROL_H
#define _TRANSLATE_CONTROL_H

#include <stdbool.h>

void translate_start(const char *server_ip, int port);
void translate_stop(void);
bool translate_is_running(void);

#endif
