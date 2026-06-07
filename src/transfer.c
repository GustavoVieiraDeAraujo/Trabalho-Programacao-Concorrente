#define _GNU_SOURCE
#include "transfer.h"
#include "database.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>

const char *MODE_LABEL[N_MODES] = {
    "Sem controle          (race condition)",
    "App: pthread_mutex_t  (exclusao mutua)",
    "App: sem_t            (semaforo POSIX)",
    "BD:  SELECT FOR UPDATE",
    "BD:  SERIALIZABLE + retry",
    "BD:  UPDATE atomico   (lock ordering)",
};

/* Short titles used in the HTML report (Portuguese) */
const char *SCENARIO_TITLE[N_MODES] = {
    "Sem Controle",
    "pthread_mutex_t",
    "sem_t — Semáforo POSIX",
    "SELECT FOR UPDATE",
    "SERIALIZABLE + retry",
    "UPDATE Atômico",
};

/* Short titles used in the HTML report (English) */
const char *SCENARIO_TITLE_EN[N_MODES] = {
    "No Control",
    "pthread_mutex_t",
    "sem_t — POSIX Semaphore",
    "SELECT FOR UPDATE",
    "SERIALIZABLE + retry",
    "Atomic UPDATE",
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t           g_sem;

void transfer_init(void)    { sem_init(&g_sem, 0, 1); }
void transfer_cleanup(void) { sem_destroy(&g_sem); }

/* ── Implementations ─────────────────────────────────────────── */

static void do_unsafe(PGconn *c, int from, int to, double amt) {
    double bf = pg_read_balance(c, from);
    double bt = pg_read_balance(c, to);
    sched_yield();                   /* widen the race window */
    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
}

static void do_mutex(PGconn *c, int from, int to, double amt) {
    pthread_mutex_lock(&g_lock);
    double bf = pg_read_balance(c, from);
    double bt = pg_read_balance(c, to);
    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
    pthread_mutex_unlock(&g_lock);
}

static void do_sem(PGconn *c, int from, int to, double amt) {
    sem_wait(&g_sem);
    double bf = pg_read_balance(c, from);
    double bt = pg_read_balance(c, to);
    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
    sem_post(&g_sem);
}

/* ORDER BY id prevents deadlocks: all threads acquire row locks in the same order. */
static void do_forupdate(PGconn *c, int from, int to, double amt) {
    int a = from < to ? from : to;
    int b = from < to ? to   : from;
    char sql[256];

    pg_exec_q(c, "BEGIN;");

    snprintf(sql, sizeof(sql),
             "SELECT id,balance FROM accounts"
             " WHERE id IN(%d,%d) ORDER BY id FOR UPDATE;", a, b);

    PGresult *r = PQexec(c, sql);
    __sync_fetch_and_add(&g_db_calls, 1);
    double bf = 0.0, bt = 0.0;
    for (int i = 0; i < PQntuples(r); i++) {
        int    id  = atoi(PQgetvalue(r, i, 0));
        double bal = atof(PQgetvalue(r, i, 1));
        if (id == from) bf = bal; else bt = bal;
    }
    PQclear(r);

    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
    pg_exec_q(c, "COMMIT;");
}

/* A serialization conflict can occur at any point; check status after each op. */
static void do_serializable(PGconn *c, int from, int to, double amt) {
    for (;;) {
        if (PQtransactionStatus(c) != PQTRANS_IDLE)
            pg_exec_q(c, "ROLLBACK;");

        pg_exec_q(c, "BEGIN ISOLATION LEVEL SERIALIZABLE;");

        double bf = pg_read_balance(c, from);
        double bt = pg_read_balance(c, to);

        if (PQtransactionStatus(c) == PQTRANS_INERROR) {
            __sync_fetch_and_add(&g_retries, 1); continue;
        }

        pg_write_balance(c, from, bf - amt);
        pg_write_balance(c, to,   bt + amt);

        if (PQtransactionStatus(c) == PQTRANS_INERROR) {
            __sync_fetch_and_add(&g_retries, 1); continue;
        }

        if (pg_exec_q(c, "COMMIT;")) break;
        __sync_fetch_and_add(&g_retries, 1);
    }
}

/* No read-modify-write: the delta is computed entirely inside PostgreSQL.
 * Lock ordering (lower id first) prevents deadlocks between concurrent transactions. */
static void do_atomic(PGconn *c, int from, int to, double amt) {
    int    first    = from < to ? from : to;
    int    second   = from < to ? to   : from;
    double d_first  = (first  == from) ? -amt : +amt;
    double d_second = (second == from) ? -amt : +amt;
    char sql[128];

    for (;;) {
        if (PQtransactionStatus(c) != PQTRANS_IDLE)
            pg_exec_q(c, "ROLLBACK;");

        pg_exec_q(c, "BEGIN;");

        snprintf(sql, sizeof(sql),
                 "UPDATE accounts SET balance=balance+%.6f WHERE id=%d;",
                 d_first, first);
        pg_exec_q(c, sql);

        snprintf(sql, sizeof(sql),
                 "UPDATE accounts SET balance=balance+%.6f WHERE id=%d;",
                 d_second, second);
        pg_exec_q(c, sql);

        if (PQtransactionStatus(c) == PQTRANS_INERROR) {
            __sync_fetch_and_add(&g_retries, 1); continue;
        }

        if (pg_exec_q(c, "COMMIT;")) break;
        __sync_fetch_and_add(&g_retries, 1);
    }
}

/* ── Dispatcher ──────────────────────────────────────────────── */
void transfer_exec(PGconn *c, Mode mode, int from, int to, double amt) {
    switch (mode) {
    case UNSAFE:          do_unsafe(c, from, to, amt);       break;
    case APP_MUTEX:       do_mutex(c, from, to, amt);        break;
    case APP_SEM:         do_sem(c, from, to, amt);          break;
    case DB_FORUPDATE:    do_forupdate(c, from, to, amt);    break;
    case DB_SERIALIZABLE: do_serializable(c, from, to, amt); break;
    case DB_ATOMIC:       do_atomic(c, from, to, amt);       break;
    default: break;
    }
}
