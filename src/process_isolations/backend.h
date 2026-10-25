#ifndef TE_STSUITE_PROCESS_ISOLATION_BACKEND_H
#define TE_STSUITE_PROCESS_ISOLATION_BACKEND_H

#include "../process_isolation.h"

/* Return the immutable operations table for each supported strategy. */
const ProcessIsolationOps *taskset_process_isolation_ops(void);
const ProcessIsolationOps *cgroupv2_process_isolation_ops(void);

#endif
