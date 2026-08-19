/*
 * POSIX semaphore stubs for wasm32-wasi.
 *
 * openh264's multi-threaded decode path references sem_* (and is compiled
 * in), but Aurora runs the decoder SINGLE-THREADED, so these are never
 * actually called at runtime. They exist only to satisfy the linker on a
 * wasi-sysroot that declares <semaphore.h> but ships no implementation.
 */
#include <semaphore.h>

int sem_init(sem_t *s, int pshared, unsigned value) {
  (void)s; (void)pshared; (void)value; return 0;
}
int sem_destroy(sem_t *s) { (void)s; return 0; }
int sem_post(sem_t *s) { (void)s; return 0; }
int sem_wait(sem_t *s) { (void)s; return 0; }
int sem_trywait(sem_t *s) { (void)s; return 0; }
int sem_timedwait(sem_t *s, const struct timespec *t) { (void)s; (void)t; return 0; }
