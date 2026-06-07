#pragma once
#include <pthread.h>

#define QUEUE_CAP 64

typedef struct {
    int id, from, to;
    double amount;
} Transfer;

typedef struct {
    Transfer        items[QUEUE_CAP];
    int             head, tail, count, done;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} WorkQueue;

void wq_init(WorkQueue *q);
void wq_destroy(WorkQueue *q);
void wq_push(WorkQueue *q, Transfer t);
int  wq_pop(WorkQueue *q, Transfer *out);  /* retorna 0 quando encerrada e vazia */
void wq_done(WorkQueue *q);
