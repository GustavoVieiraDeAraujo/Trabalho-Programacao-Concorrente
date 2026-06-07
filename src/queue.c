#include "queue.h"

void wq_init(WorkQueue *q) {
    q->head = q->tail = q->count = q->done = 0;
    pthread_mutex_init(&q->lock,      NULL);
    pthread_cond_init(&q->not_empty,  NULL);
    pthread_cond_init(&q->not_full,   NULL);
}

void wq_destroy(WorkQueue *q) {
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

void wq_push(WorkQueue *q, Transfer t) {
    pthread_mutex_lock(&q->lock);
    while (q->count == QUEUE_CAP)
        pthread_cond_wait(&q->not_full, &q->lock);
    q->items[q->tail] = t;
    q->tail = (q->tail + 1) % QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}

int wq_pop(WorkQueue *q, Transfer *out) {
    pthread_mutex_lock(&q->lock);
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    *out    = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

void wq_done(WorkQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
}
