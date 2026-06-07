#define _GNU_SOURCE
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

volatile int g_db_calls = 0;
volatile int g_retries  = 0;

PGconn *pg_connect(void) {
    PGconn *c = PQconnectdb(PG_CONNSTR);
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr, "Connection failed: %s\n", PQerrorMessage(c));
        PQfinish(c);
        return NULL;
    }
    return c;
}

/* Executes SQL; returns 1 on success, 0 on error. */
int pg_exec_q(PGconn *c, const char *sql) {
    PGresult *r  = PQexec(c, sql);
    int       ok = (PQresultStatus(r) == PGRES_COMMAND_OK ||
                    PQresultStatus(r) == PGRES_TUPLES_OK);
    PQclear(r);
    __sync_fetch_and_add(&g_db_calls, 1);
    return ok;
}

/* Executes SQL and prints an error message on failure. */
void pg_exec(PGconn *c, const char *sql) {
    if (!pg_exec_q(c, sql))
        fprintf(stderr, "SQL [%.40s]: %s\n", sql, PQerrorMessage(c));
}

/* Returns the balance of account id. */
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

/* Overwrites the balance of account id. */
void pg_write_balance(PGconn *c, int id, double val) {
    char sql[128];
    snprintf(sql, sizeof(sql),
             "UPDATE accounts SET balance=%.6f WHERE id=%d;", val, id);
    PGresult *r = PQexec(c, sql);
    __sync_fetch_and_add(&g_db_calls, 1);
    PQclear(r);
}

/* Drops and recreates the accounts table, resetting every balance to INIT_BAL. */
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
    g_db_calls = 0;
    g_retries  = 0;
}

/* Returns the sum of all account balances. */
double db_total(PGconn *conn) {
    PGresult *r   = PQexec(conn, "SELECT SUM(balance) FROM accounts;");
    double    sum = (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r))
                       ? atof(PQgetvalue(r, 0, 0)) : 0.0;
    PQclear(r);
    return sum;
}
