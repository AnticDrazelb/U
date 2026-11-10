/* xbox-core: QEMU's big lock (BQL). QEMU holds it around device register accesses and its device
 * code takes and drops it (xemu's PFIFO thread, interrupt updates); xbox-core's adapters take it
 * around each register access of xemu's NV2A. */
#include <pthread.h>
static pthread_mutex_t g_bql = PTHREAD_MUTEX_INITIALIZER;
void xb_bql_lock(void) { pthread_mutex_lock(&g_bql); }
void xb_bql_unlock(void) { pthread_mutex_unlock(&g_bql); }
