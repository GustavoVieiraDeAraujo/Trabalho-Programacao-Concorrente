#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include "database.h"
#include "queue.h"
#include "transfer.h"
#include "result.h"
#include "report.h"

#define MAIN_THREADS   8
#define MAIN_TRANSFERS 500
#define TRANSF_MAX     100.0
#define SCALE_TR       300

static const int  SCALE_THREADS[] = {2, 4, 8, 16};
static const Mode SCALE_MODES[]   = { UNSAFE, APP_MUTEX, DB_FORUPDATE, DB_ATOMIC };
#define SCALE_N  (int)(sizeof(SCALE_THREADS)/sizeof(SCALE_THREADS[0]))
#define SCALE_MN (int)(sizeof(SCALE_MODES)/sizeof(SCALE_MODES[0]))

/* ── Producer ────────────────────────────────────────────────── */
typedef struct { int n; unsigned int seed; WorkQueue *q; } ProducerArgs;

static void *producer_fn(void *arg) {
    ProducerArgs *a = arg;
    for (int i = 0; i < a->n; i++) {
        int from = rand_r(&a->seed) % NUM_ACCTS;
        int to;
        do { to = rand_r(&a->seed) % NUM_ACCTS; } while (to == from);
        wq_push(a->q, (Transfer){ i, from, to,
            (double)(rand_r(&a->seed) % (int)TRANSF_MAX) + 1.0 });
    }
    wq_done(a->q);
    return NULL;
}

/* ── Worker ──────────────────────────────────────────────────── */
typedef struct { Mode mode; WorkQueue *q; } WorkerArgs;

static void *worker_fn(void *arg) {
    WorkerArgs *a    = arg;
    PGconn     *conn = pg_connect();
    if (!conn) return NULL;
    Transfer t;
    while (wq_pop(a->q, &t))
        transfer_exec(conn, a->mode, t.from, t.to, t.amount);
    PQfinish(conn);
    return NULL;
}

/* ── Single scenario run ─────────────────────────────────────── */
static Result run_scenario(PGconn *mc, Mode mode, int n_thr, int n_tr) {
    db_reset(mc);
    double before = db_total(mc);

    WorkQueue    wq;
    WorkerArgs   work_args = { mode, &wq };
    ProducerArgs prod_args = { n_tr, 42, &wq };
    pthread_t    producer, workers[n_thr];

    wq_init(&wq);
    transfer_init();

    pthread_create(&producer, NULL, producer_fn, &prod_args);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < n_thr; i++)
        pthread_create(&workers[i], NULL, worker_fn, &work_args);
    pthread_join(producer, NULL);
    for (int i = 0; i < n_thr; i++)
        pthread_join(workers[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    transfer_cleanup();
    wq_destroy(&wq);

    double after = db_total(mc);
    return (Result){
        .initial     = before,
        .final       = after,
        .discrepancy = after - before,
        .n_transfers = n_tr,
        .db_calls    = g_db_calls,
        .retries     = g_retries,
        .ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
              (t1.tv_nsec - t0.tv_nsec) / 1e6,
    };
}

/* ── Scalability data collection ─────────────────────────────── */
static void compute_scaling(PGconn *mc,
                             double tps[SCALE_MN][SCALE_N],
                             int    ok [SCALE_MN][SCALE_N]) {
    printf("  Scaling test (%d x %d runs)...\n", SCALE_MN, SCALE_N);
    for (int i = 0; i < SCALE_MN; i++)
        for (int j = 0; j < SCALE_N; j++) {
            printf("    %s — %d threads\n",
                   MODE_LABEL[SCALE_MODES[i]], SCALE_THREADS[j]);
            Result r  = run_scenario(mc, SCALE_MODES[i], SCALE_THREADS[j], SCALE_TR);
            ok [i][j] = (r.discrepancy > -0.01 && r.discrepancy < 0.01);
            tps[i][j] = r.n_transfers / (r.ms / 1000.0);
        }
}

/* ── Emit one full report in the given language ──────────────── */
static void emit_report(Lang lang,
                         int  threads, int transfers,
                         const Result results[],
                         double tps[SCALE_MN][SCALE_N],
                         int    ok [SCALE_MN][SCALE_N]) {
    report_open(lang);
    report_header(threads, transfers);
    for (int m = 0; m < N_MODES; m++)
        report_scenario(m + 1, (Mode)m, &results[m]);
    report_summary(results, N_MODES);
    report_scaling(SCALE_MODES, SCALE_MN, SCALE_THREADS, SCALE_N, tps, ok);
    report_conclusion(results, N_MODES);
    report_close();
}

/* ── Main ────────────────────────────────────────────────────── */
int main(void) {
    PGconn *mc = pg_connect();
    if (!mc) return 1;

    /* ── 1. Run all scenarios and collect results ── */
    printf("Running %d scenarios (%d transfers each)...\n",
           N_MODES, MAIN_TRANSFERS);

    Result results[N_MODES];
    for (int m = 0; m < N_MODES; m++) {
        printf("  [%d/%d] %s\n", m + 1, N_MODES, MODE_LABEL[m]);
        results[m] = run_scenario(mc, (Mode)m, MAIN_THREADS, MAIN_TRANSFERS);
    }

    /* ── 2. Collect scalability data ── */
    double scale_tps[SCALE_MN][SCALE_N];
    int    scale_ok [SCALE_MN][SCALE_N];
    compute_scaling(mc, scale_tps, scale_ok);

    /* ── 3. Generate reports in both languages ── */
    emit_report(LANG_PT, MAIN_THREADS, MAIN_TRANSFERS, results, scale_tps, scale_ok);
    emit_report(LANG_EN, MAIN_THREADS, MAIN_TRANSFERS, results, scale_tps, scale_ok);

    PQfinish(mc);
    return 0;
}
