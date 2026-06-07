#pragma once
#include <libpq-fe.h>

#define PG_CONNSTR "host=localhost port=5433 dbname=banco user=banco password=banco"
#define NUM_ACCTS  10
#define INIT_BAL   1000.0

extern volatile int g_db_calls;
extern volatile int g_retries;

PGconn *pg_connect(void);
int     pg_exec_q(PGconn *c, const char *sql);
void    pg_exec(PGconn *c, const char *sql);
double  pg_read_balance(PGconn *c, int id);
void    pg_write_balance(PGconn *c, int id, double val);
void    db_reset(PGconn *conn);
double  db_total(PGconn *conn);
