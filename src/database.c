/*
 * database.c
 *
 * Camada de acesso ao PostgreSQL: conexão, queries básicas, reset e leitura
 * do total de saldos. Todas as funções são thread-safe no sentido de que cada
 * thread usa sua própria PGconn — não há estado compartilhado aqui, exceto
 * pelos contadores g_db_calls e g_retries, que são incrementados atomicamente.
 */

#define _GNU_SOURCE
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Contadores globais compartilhados por todas as threads.
 * Incrementados com __sync_fetch_and_add (GCC built-in, operação atômica)
 * para evitar race condition nos próprios contadores de instrumentação.
 * São zerados em db_reset() antes de cada cenário.
 */
volatile int g_db_calls = 0;
volatile int g_retries  = 0;

/*
 * Abre uma conexão com o PostgreSQL usando a string de conexão definida
 * em database.h. Cada worker deve abrir sua própria conexão — conexões
 * compartilhadas serializariam as operações no lado do cliente e mascariam
 * as race conditions que o experimento quer medir.
 */
PGconn *pg_connect(void) {
    PGconn *c = PQconnectdb(PG_CONNSTR);
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr, "Falha na conexao: %s\n", PQerrorMessage(c));
        PQfinish(c);
        return NULL;
    }
    return c;
}

/*
 * Executa um comando SQL e retorna 1 em caso de sucesso, 0 em caso de erro.
 * Versão "quieta": não imprime mensagem de erro (útil quando o chamador
 * precisa tratar o retorno, como no loop de retry do modo SERIALIZABLE).
 */
int pg_exec_q(PGconn *c, const char *sql) {
    PGresult *r  = PQexec(c, sql);
    int       ok = (PQresultStatus(r) == PGRES_COMMAND_OK ||
                    PQresultStatus(r) == PGRES_TUPLES_OK);
    PQclear(r);
    __sync_fetch_and_add(&g_db_calls, 1);
    return ok;
}

/*
 * Executa um comando SQL e imprime uma mensagem de erro se a query falhar.
 * Usada para operações que não devem falhar em circunstâncias normais
 * (BEGIN, COMMIT, INSERT no db_reset, etc.).
 */
void pg_exec(PGconn *c, const char *sql) {
    if (!pg_exec_q(c, sql))
        fprintf(stderr, "SQL [%.40s]: %s\n", sql, PQerrorMessage(c));
}

/*
 * Lê e retorna o saldo atual da conta com o id informado.
 * Não envolve transação explícita — se chamada fora de um BEGIN/COMMIT,
 * o banco usa o nível de isolamento padrão (READ COMMITTED), o que é
 * suficiente para os modos UNSAFE, MUTEX e SEM, e insuficiente para os
 * modos de banco (que gerenciam suas próprias transações).
 */
double pg_read_balance(PGconn *c, int id) {
    char sql[64];
    snprintf(sql, sizeof(sql), "SELECT balance FROM accounts WHERE id=%d;", id);
    PGresult *r = PQexec(c, sql);
    __sync_fetch_and_add(&g_db_calls, 1);
    double val = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r))
                     ? atof(PQgetvalue(r, 0, 0)) : 0.0;
    PQclear(r);
    return val;
}

/*
 * Sobrescreve o saldo da conta com o id informado.
 * Junto com pg_read_balance, forma o padrão read-modify-write que cria
 * a janela de race condition nos modos UNSAFE, MUTEX e SEM.
 */
void pg_write_balance(PGconn *c, int id, double val) {
    char sql[128];
    snprintf(sql, sizeof(sql),
             "UPDATE accounts SET balance=%.6f WHERE id=%d;", val, id);
    PGresult *r = PQexec(c, sql);
    __sync_fetch_and_add(&g_db_calls, 1);
    PQclear(r);
}

/*
 * Recria a tabela accounts do zero e insere NUM_ACCTS contas com saldo
 * INIT_BAL cada. Chamada antes de cada cenário para garantir que todos
 * partem do mesmo estado inicial (total = NUM_ACCTS * INIT_BAL).
 * Zera também os contadores de chamadas e retries.
 */
void db_reset(PGconn *conn) {
    pg_exec(conn, "SET client_min_messages=WARNING;");
    pg_exec(conn, "DROP TABLE IF EXISTS accounts;");
    pg_exec(conn, "CREATE TABLE accounts("
                  "  id      INTEGER PRIMARY KEY,"
                  "  balance DOUBLE PRECISION NOT NULL);");
    char sql[128];
    for (int i = 0; i < NUM_ACCTS; i++) {
        snprintf(sql, sizeof(sql),
                 "INSERT INTO accounts(id,balance) VALUES(%d,%.2f);", i, INIT_BAL);
        pg_exec(conn, sql);
    }
    /* reinicia contadores para medir cada cenário de forma isolada */
    g_db_calls = 0;
    g_retries  = 0;
}

/*
 * Retorna a soma de todos os saldos da tabela accounts.
 * É o indicador de corretude do experimento: se o resultado diferir do
 * total inicial, significa que dinheiro foi criado ou destruído por uma
 * race condition durante a execução do cenário.
 */
double db_total(PGconn *conn) {
    PGresult *r   = PQexec(conn, "SELECT SUM(balance) FROM accounts;");
    double    sum = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r))
                       ? atof(PQgetvalue(r, 0, 0)) : 0.0;
    PQclear(r);
    return sum;
}
