/*
 * main.c
 *
 * Ponto de entrada e orquestração do experimento.
 *
 * Fluxo principal:
 *   1. Executa cada um dos N_MODES cenários com MAIN_THREADS workers e
 *      MAIN_TRANSFERS transferências, coletando métricas de cada um.
 *   2. Roda o teste de escalabilidade: mesmas configurações com 2, 4, 8
 *      e 16 threads para mostrar como cada mecanismo se comporta sob carga.
 *   3. Gera os relatórios HTML/PDF em português e inglês.
 */

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

/* Configuração dos cenários principais */
#define MAIN_THREADS   8      /* número de workers para os cenários principais */
#define MAIN_TRANSFERS 500    /* transferências por cenário */
#define TRANSF_MAX     100.0  /* valor máximo de cada transferência (R$) */
#define SCALE_TR       300    /* transferências usadas nos testes de escalabilidade */

/* Combinações avaliadas no teste de escalabilidade */
static const int  SCALE_THREADS[] = {2, 4, 8, 16};
static const Mode SCALE_MODES[]   = { UNSAFE, APP_MUTEX, DB_FORUPDATE, DB_ATOMIC };
#define SCALE_N  (int)(sizeof(SCALE_THREADS)/sizeof(SCALE_THREADS[0]))
#define SCALE_MN (int)(sizeof(SCALE_MODES)/sizeof(SCALE_MODES[0]))

/* ── Produtor ────────────────────────────────────────────────── */

/* Argumentos passados à thread produtora */
typedef struct { int n; unsigned int seed; WorkQueue *q; } ProducerArgs;

/*
 * Gera n transferências aleatórias e as coloca na fila de trabalho.
 * Usa rand_r (reentrante e thread-safe) com semente fixa para que as
 * sequências de transferências sejam idênticas em todos os cenários,
 * tornando a comparação entre mecanismos justa.
 */
static void *producer_fn(void *arg) {
    ProducerArgs *a = arg;
    for (int i = 0; i < a->n; i++) {
        int from = rand_r(&a->seed) % NUM_ACCTS;
        int to;
        /* garante contas distintas: transferências para si mesmo não fazem sentido */
        do { to = rand_r(&a->seed) % NUM_ACCTS; } while (to == from);
        wq_push(a->q, (Transfer){ i, from, to,
            (double)(rand_r(&a->seed) % (int)TRANSF_MAX) + 1.0 });
    }
    /* informa aos workers que não haverá mais itens */
    wq_done(a->q);
    return NULL;
}

/* ── Worker ──────────────────────────────────────────────────── */

/* Argumentos passados a cada thread worker */
typedef struct { Mode mode; WorkQueue *q; } WorkerArgs;

/*
 * Consome transferências da fila até ela ser encerrada e esvaziada.
 * Cada worker abre sua própria conexão ao banco — isso é intencional:
 * conexões compartilhadas serializariam as queries automaticamente,
 * mascarando as race conditions que o experimento precisa revelar.
 */
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

/* ── Execução de um cenário completo ─────────────────────────── */

/*
 * Reseta o banco, inicia o produtor e os workers, aguarda o término de todos
 * e retorna as métricas coletadas.
 *
 * A discrepância (final − inicial) é o indicador central: qualquer valor
 * diferente de zero significa que dinheiro foi criado ou destruído por uma
 * race condition — mesmo que o programa tenha rodado sem erros visíveis.
 */
static Result run_scenario(PGconn *mc, Mode mode, int n_thr, int n_tr) {
    db_reset(mc);
    double before = db_total(mc); /* deve ser NUM_ACCTS * INIT_BAL */

    WorkQueue    wq;
    WorkerArgs   work_args = { mode, &wq };
    ProducerArgs prod_args = { n_tr, 42, &wq }; /* semente 42 → sequência reproduzível */
    pthread_t    producer, workers[n_thr];

    wq_init(&wq);
    transfer_init(); /* inicializa o semáforo POSIX usado pelo modo APP_SEM */

    pthread_create(&producer, NULL, producer_fn, &prod_args);

    /* a medição de tempo cobre apenas a execução dos workers, excluindo
       o tempo de inicialização da fila e do produtor */
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
        .discrepancy = after - before,  /* ≠ 0 → race condition detectada */
        .n_transfers = n_tr,
        .db_calls    = g_db_calls,
        .retries     = g_retries,
        .ms = (t1.tv_sec - t0.tv_sec) * 1e3 +
              (t1.tv_nsec - t0.tv_nsec) / 1e6,
    };
}

/* ── Teste de escalabilidade ─────────────────────────────────── */

/*
 * Executa cada combinação (modo × número de threads) e registra o TPS
 * e se o resultado foi correto. Alimenta a tabela de escalabilidade do relatório.
 */
static void compute_scaling(PGconn *mc,
                             double tps[SCALE_MN][SCALE_N],
                             int    ok [SCALE_MN][SCALE_N]) {
    printf("  Teste de escalabilidade (%d x %d execucoes)...\n", SCALE_MN, SCALE_N);
    for (int i = 0; i < SCALE_MN; i++)
        for (int j = 0; j < SCALE_N; j++) {
            printf("    %s — %d threads\n",
                   MODE_LABEL[SCALE_MODES[i]], SCALE_THREADS[j]);
            Result r  = run_scenario(mc, SCALE_MODES[i], SCALE_THREADS[j], SCALE_TR);
            ok [i][j] = (r.discrepancy > -0.01 && r.discrepancy < 0.01);
            tps[i][j] = r.n_transfers / (r.ms / 1000.0);
        }
}

/* ── Geração de relatório num idioma ─────────────────────────── */

/*
 * Percorre a API de relatório em sequência, acumulando cada seção em
 * buffers de memória. report_close() substitui os placeholders no template
 * HTML e salva o arquivo final.
 */
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

/* ── Ponto de entrada ────────────────────────────────────────── */
int main(void) {
    /* conexão principal: usada apenas para db_reset e db_total, não para transferências */
    PGconn *mc = pg_connect();
    if (!mc) return 1;

    /* 1. Executa todos os cenários e coleta resultados */
    printf("Executando %d cenarios (%d transferencias cada)...\n",
           N_MODES, MAIN_TRANSFERS);

    Result results[N_MODES];
    for (int m = 0; m < N_MODES; m++) {
        printf("  [%d/%d] %s\n", m + 1, N_MODES, MODE_LABEL[m]);
        results[m] = run_scenario(mc, (Mode)m, MAIN_THREADS, MAIN_TRANSFERS);
    }

    /* 2. Coleta dados de escalabilidade (modo × número de threads) */
    double scale_tps[SCALE_MN][SCALE_N];
    int    scale_ok [SCALE_MN][SCALE_N];
    compute_scaling(mc, scale_tps, scale_ok);

    /* 3. Gera relatórios em português e inglês */
    emit_report(LANG_PT, MAIN_THREADS, MAIN_TRANSFERS, results, scale_tps, scale_ok);
    emit_report(LANG_EN, MAIN_THREADS, MAIN_TRANSFERS, results, scale_tps, scale_ok);

    PQfinish(mc);
    return 0;
}
