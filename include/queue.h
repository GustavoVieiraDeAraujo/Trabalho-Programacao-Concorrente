/*
 * queue.h
 *
 * Fila de trabalho circular com controle de concorrência (bounded buffer).
 * Usada no padrão produtor-consumidor: um produtor empurra transferências
 * e N workers as consomem em paralelo, cada um com sua própria conexão ao banco.
 */

#pragma once
#include <pthread.h>

/* Capacidade máxima da fila (número de itens simultâneos no buffer) */
#define QUEUE_CAP 64

/* Representa uma transferência bancária a ser executada por um worker */
typedef struct {
    int    id;     /* identificador sequencial da transferência */
    int    from;   /* id da conta de origem */
    int    to;     /* id da conta de destino */
    double amount; /* valor a transferir (R$) */
} Transfer;

/*
 * Fila circular protegida por mutex + duas variáveis condição.
 *
 * Invariante: head é o próximo índice a ser lido; tail é o próximo índice
 * a ser escrito; count é o número de itens presentes. A condição
 * "fila encerrada e vazia" (done == 1 && count == 0) sinaliza término.
 */
typedef struct {
    Transfer        items[QUEUE_CAP]; /* buffer circular de itens */
    int             head, tail;       /* índices de leitura e escrita (avançam em módulo QUEUE_CAP) */
    int             count;            /* número de itens presentes no momento */
    int             done;             /* 1 quando o produtor não vai mais inserir itens */
    pthread_mutex_t lock;             /* mutex que protege todos os campos acima */
    pthread_cond_t  not_empty;        /* sinalizada ao inserir: acorda workers bloqueados */
    pthread_cond_t  not_full;         /* sinalizada ao remover: acorda o produtor bloqueado */
} WorkQueue;

void wq_init(WorkQueue *q);            /* inicializa mutex, condições e índices */
void wq_destroy(WorkQueue *q);         /* destrói mutex e condições */
void wq_push(WorkQueue *q, Transfer t);/* insere item; bloqueia o produtor se a fila estiver cheia */
int  wq_pop(WorkQueue *q, Transfer *out); /* remove item; retorna 0 quando encerrada e vazia */
void wq_done(WorkQueue *q);            /* marca fim da produção; acorda todos os workers */
