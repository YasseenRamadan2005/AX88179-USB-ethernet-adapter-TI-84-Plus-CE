#ifndef DRIVER_H
#define DRIVER_H

#include <stdarg.h>

struct gui;

void debug_log(struct gui *gui, const char *fmt, ...);

#endif