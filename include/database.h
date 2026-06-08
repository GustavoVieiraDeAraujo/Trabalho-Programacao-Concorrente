/*
 * database.h
 *
 * Interface de acesso ao PostgreSQL: constantes de conexão, contadores
 * globais de instrumentação e declarações das funções de banco.
 */

#pragma once
#include <libpq-fe.h>

/* String de conexão com o PostgreSQL em Docker (porta 5433 evita conflito com instâncias locais) */
#define PG_CONNSTR "host=localhost port=5433 dbname=banco user=banco password=banco"

/* Número de contas bancárias simuladas */
#define NUM_ACCTS  10

/* Saldo inicial de cada conta (R$) */
#define INIT_BAL   1000.0

/*
 * Contadores globais incrementados atomicamente por todas as threads.
 * g_db_calls: total de chamadas ao banco no cenário atual.
 * g_retries:  número de retentativas no modo SERIALIZABLE.
 * Ambos são zerados em db_reset() antes de cada cenário.
 */
extern volatile int g_db_calls;
extern volatile int g_retries;

PGconn *pg_connect(void);                        /* abre conexão com o PostgreSQL */
int     pg_exec_q(PGconn *c, const char *sql);   /* executa SQL; retorna 1=ok, 0=erro (sem mensagem) */
void    pg_exec(PGconn *c, const char *sql);     /* executa SQL; imprime erro em caso de falha */
double  pg_read_balance(PGconn *c, int id);      /* lê saldo da conta id */
void    pg_write_balance(PGconn *c, int id, double val); /* sobrescreve saldo da conta id */
void    db_reset(PGconn *conn);                  /* recria a tabela com saldos iniciais */
double  db_total(PGconn *conn);                  /* retorna soma de todos os saldos */
