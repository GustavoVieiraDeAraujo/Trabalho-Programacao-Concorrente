/*
 * transfer.h
 *
 * Define os modos de sincronização disponíveis e declara a interface
 * de execução de transferências bancárias.
 */

#pragma once
#include <libpq-fe.h>

/*
 * Modos de sincronização avaliados no experimento, em ordem crescente
 * de sofisticação: do sem controle (race condition intencional) até o
 * UPDATE atômico (melhor desempenho com corretude garantida).
 */
typedef enum {
    UNSAFE = 0,       /* sem proteção — demonstra race condition (lost update) */
    APP_MUTEX,        /* pthread_mutex_t global — exclusão mútua na aplicação */
    APP_SEM,          /* sem_t POSIX binário — equivalente ao mutex neste contexto */
    DB_FORUPDATE,     /* SELECT FOR UPDATE — lock de linha no banco + BEGIN/COMMIT */
    DB_SERIALIZABLE,  /* SERIALIZABLE — o banco detecta e aborta conflitos; aplicação faz retry */
    DB_ATOMIC,        /* UPDATE atômico — sem read-modify-write; cálculo feito pelo banco */
    N_MODES           /* sentinela: número total de modos (não é um modo válido) */
} Mode;

extern const char *MODE_LABEL[N_MODES];        /* rótulos completos para o terminal */
extern const char *SCENARIO_TITLE[N_MODES];    /* títulos curtos para o relatório em português */
extern const char *SCENARIO_TITLE_EN[N_MODES]; /* títulos curtos para o relatório em inglês */

/* Inicializa o semáforo POSIX compartilhado; deve ser chamado antes de cada cenário */
void transfer_init(void);

/* Destrói o semáforo POSIX; deve ser chamado após cada cenário */
void transfer_cleanup(void);

/*
 * Executa a transferência de 'amt' reais da conta 'from' para a conta 'to'
 * usando o mecanismo de sincronização indicado por 'mode'.
 * Cada chamada pode resultar em múltiplas queries ao banco dependendo do modo.
 */
void transfer_exec(PGconn *c, Mode mode, int from, int to, double amt);
