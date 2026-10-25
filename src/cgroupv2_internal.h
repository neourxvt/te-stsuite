#ifndef TE_STSUITE_CGROUPV2_INTERNAL_H
#define TE_STSUITE_CGROUPV2_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

/* Share basic cgroup path and control-file operations across backends. */
char *cgroupv2_child_path(const char *parent, const char *name);
bool cgroupv2_write_control(const char *path, const char *value);

#endif
