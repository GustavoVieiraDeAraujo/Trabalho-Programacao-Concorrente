/*
 * queue.c
 *
 * Implementação da fila de trabalho circular com controle de concorrência.
 *
 * Padrão produtor-consumidor com buffer limitado (bounded buffer):
 *   - Um único produtor empurra transferências com wq_push().
 *   - N workers consomem transferências com wq_pop().
 *   - A fila tem capacidade QUEUE_CAP: o produtor bloqueia quando cheia
 *     (back-pressure) e os workers bloqueiam quando vazia.
 *
 * Sincronização:
 *   - Um mutex protege todos os campos da struct.
 *   - not_empty: sinalizada quando um item é inserido (acorda workers).
 *   - not_full:  sinalizada quando um item é removido (acorda o produtor).
 */

#include "queue.h"

/*
 * Inicializa a fila: zera todos os índices e contadores, cria o mutex e
 * as duas variáveis condição com atributos padrão.
 */
void wq_init(WorkQueue *q) {
    q->head = q->tail = q->count = q->done = 0;
    pthread_mutex_init(&q->lock,     NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full,  NULL);
}

/* Destrói o mutex e as variáveis condição, liberando os recursos do SO. */
void wq_destroy(WorkQueue *q) {
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/*
 * Insere um item no final da fila (posição tail).
 *
 * Se a fila estiver cheia (count == QUEUE_CAP), bloqueia o chamador até
 * que algum worker libere espaço. pthread_cond_wait libera o mutex de forma
 * atômica ao dormir e o readquire ao acordar — isso evita a condição em que
 * a sinalização ocorre entre o teste da condição e o sono.
 *
 * Após inserir, sinaliza not_empty para acordar um worker que esteja
 * esperando por itens.
 */
void wq_push(WorkQueue *q, Transfer t) {
    pthread_mutex_lock(&q->lock);
    /* espera até haver espaço: bloqueia o produtor enquanto a fila estiver cheia */
    while (q->count == QUEUE_CAP)
        pthread_cond_wait(&q->not_full, &q->lock);
    q->items[q->tail] = t;
    q->tail = (q->tail + 1) % QUEUE_CAP; /* avança em anel: após o último slot volta ao zero */
    q->count++;
    pthread_cond_signal(&q->not_empty); /* acorda um worker que esteja aguardando */
    pthread_mutex_unlock(&q->lock);
}

/*
 * Remove e entrega o próximo item da fila (posição head).
 *
 * Retorna 1 se um item foi entregue em *out.
 * Retorna 0 se a fila está vazia E o produtor já sinalizou fim (done == 1).
 * Workers devem interpretar retorno 0 como sinal para encerrar.
 *
 * O laço while (em vez de if) protege contra spurious wakeups: o POSIX
 * permite que pthread_cond_wait retorne sem sinalização, então a condição
 * sempre deve ser reavaliada após o retorno.
 */
int wq_pop(WorkQueue *q, Transfer *out) {
    pthread_mutex_lock(&q->lock);
    /* aguarda item OU sinalização de encerramento */
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->lock);
    if (q->count == 0) {
        /* fila vazia e encerrada: informa o worker para finalizar */
        pthread_mutex_unlock(&q->lock);
        return 0;
    }
    *out    = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAP; /* avança em anel */
    q->count--;
    pthread_cond_signal(&q->not_full); /* notifica o produtor caso esteja bloqueado */
    pthread_mutex_unlock(&q->lock);
    return 1;
}

/*
 * Marca a fila como encerrada: nenhum novo item será inserido.
 *
 * Usa pthread_cond_broadcast (não signal) para acordar TODOS os workers
 * simultaneamente. Isso é necessário porque, com múltiplos workers
 * bloqueados em not_empty, signal acordaria apenas um — os demais
 * ficariam esperando indefinidamente mesmo com a fila encerrada.
 *
 * Deve ser chamado pelo produtor após wq_push do último item.
 */
void wq_done(WorkQueue *q) {
    pthread_mutex_lock(&q->lock);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty); /* acorda TODOS os workers para que terminem */
    pthread_mutex_unlock(&q->lock);
}
