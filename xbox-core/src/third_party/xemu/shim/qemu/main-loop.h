/* xbox-core: QEMU big lock, a real one (shim/xb_bql.c) */
void xb_bql_lock(void);
void xb_bql_unlock(void);
#define bql_lock() xb_bql_lock()
#define bql_unlock() xb_bql_unlock()
