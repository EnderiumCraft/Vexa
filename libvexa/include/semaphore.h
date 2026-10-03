#ifndef LIBVEXA_SEMAPHORE_H
#define LIBVEXA_SEMAPHORE_H

#include <time.h>

typedef struct {
    volatile unsigned value;
    volatile unsigned waiters;
} sem_t;

int sem_init(sem_t *sem, int shared, unsigned value);
int sem_destroy(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
int sem_timedwait(sem_t *sem, const struct timespec *deadline);
int sem_post(sem_t *sem);
int sem_getvalue(sem_t *sem, int *value);

#endif
