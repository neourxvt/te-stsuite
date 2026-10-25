#ifndef TE_STSUITE_PROCESS_ISOLATION_H
#define TE_STSUITE_PROCESS_ISOLATION_H

#include <stdbool.h>

/* Pin the calling process to one CPU before it launches the terminal. */
bool process_isolation_pin_cpu(unsigned cpu);

#endif
