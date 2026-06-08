/*
 * result.h
 *
 * Estrutura que agrega as métricas coletadas ao final de cada cenário.
 * A discrepância é o indicador central de corretude: qualquer valor
 * diferente de zero significa que dinheiro foi criado ou destruído por
 * uma race condition durante a execução.
 */

#pragma once

typedef struct {
    double initial;      /* soma dos saldos antes da execução — deve ser NUM_ACCTS * INIT_BAL */
    double final;        /* soma dos saldos após a execução */
    double discrepancy;  /* final − initial: zero = dados corretos; ≠ 0 = race condition */
    int    n_transfers;  /* número de transferências realizadas no cenário */
    int    db_calls;     /* total de chamadas ao banco (SELECT + UPDATE + BEGIN + COMMIT) */
    int    retries;      /* retentativas de transação (relevante apenas no modo SERIALIZABLE) */
    double ms;           /* tempo total de execução dos workers em milissegundos */
} Result;
