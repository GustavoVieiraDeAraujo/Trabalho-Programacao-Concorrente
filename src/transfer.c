/*
 * transfer.c
 *
 * Implementa os seis mecanismos de sincronização avaliados no experimento.
 * Cada função do_*() realiza a mesma operação lógica (transferir 'amt' reais
 * de 'from' para 'to'), mas com estratégias de proteção diferentes.
 *
 * A função transfer_exec() serve como despachante: recebe o enum Mode e
 * chama a implementação correspondente.
 */

#define _GNU_SOURCE
#include "transfer.h"
#include "database.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>

/* Rótulos exibidos no terminal durante a execução dos cenários */
const char *MODE_LABEL[N_MODES] = {
    "Sem controle          (race condition)",
    "App: pthread_mutex_t  (exclusao mutua)",
    "App: sem_t            (semaforo POSIX)",
    "BD:  SELECT FOR UPDATE",
    "BD:  SERIALIZABLE + retry",
    "BD:  UPDATE atomico   (lock ordering)",
};

/* Títulos curtos usados no relatório HTML (português) */
const char *SCENARIO_TITLE[N_MODES] = {
    "Sem Controle",
    "pthread_mutex_t",
    "sem_t — Semáforo POSIX",
    "SELECT FOR UPDATE",
    "SERIALIZABLE + retry",
    "UPDATE Atômico",
};

/* Títulos curtos usados no relatório HTML (inglês) */
const char *SCENARIO_TITLE_EN[N_MODES] = {
    "No Control",
    "pthread_mutex_t",
    "sem_t — POSIX Semaphore",
    "SELECT FOR UPDATE",
    "SERIALIZABLE + retry",
    "Atomic UPDATE",
};

/*
 * Primitivos de sincronização globais, compartilhados por todos os workers.
 * São globais para reproduzir o cenário realista em que múltiplas threads
 * disputam acesso ao mesmo recurso. O mutex é inicializado estaticamente
 * (PTHREAD_MUTEX_INITIALIZER); o semáforo precisa de sem_init/sem_destroy.
 */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t           g_sem;

/* Inicializa o semáforo binário (valor 1 = uma "vaga").
   Chamado antes de cada cenário para garantir estado limpo. */
void transfer_init(void)    { sem_init(&g_sem, 0, 1); }

/* Destrói o semáforo ao final de cada cenário. */
void transfer_cleanup(void) { sem_destroy(&g_sem); }

/* ── Implementações ──────────────────────────────────────────── */

/*
 * CENÁRIO 1 — Sem controle (race condition intencional)
 *
 * Demonstra o padrão "lost update": Thread A lê o saldo, cede a CPU
 * (sched_yield), Thread B lê o mesmo saldo desatualizado e os dois
 * sobrescrevem um ao outro. sched_yield() amplia propositalmente a janela
 * de tempo entre leitura e escrita, tornando a corrupção sempre observável.
 */
static void do_unsafe(PGconn *c, int from, int to, double amt) {
    double bf = pg_read_balance(c, from);
    double bt = pg_read_balance(c, to);
    sched_yield(); /* cede a CPU para aumentar a chance de interleaving entre threads */
    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
}

/*
 * CENÁRIO 2 — pthread_mutex_t (exclusão mútua na aplicação)
 *
 * Um mutex global serializa toda a seção crítica: apenas uma thread executa
 * o bloco leitura-cálculo-escrita por vez. Garante corretude, mas elimina
 * o paralelismo: threads em contas distintas também se bloqueiam mutuamente,
 * porque há um único lock para todas as transferências.
 */
static void do_mutex(PGconn *c, int from, int to, double amt) {
    pthread_mutex_lock(&g_lock);   /* entra na seção crítica */
    double bf = pg_read_balance(c, from);
    double bt = pg_read_balance(c, to);
    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
    pthread_mutex_unlock(&g_lock); /* libera para a próxima thread */
}

/*
 * CENÁRIO 3 — sem_t / Semáforo POSIX (exclusão mútua na aplicação)
 *
 * Comportamento idêntico ao mutex neste contexto (semáforo binário = mutex).
 * A diferença conceitual é que sem_post pode ser chamado por uma thread
 * diferente da que chamou sem_wait — o que é fundamental no padrão
 * produtor-consumidor (WorkQueue), mas irrelevante aqui onde a mesma
 * thread que "entra" também "sai".
 */
static void do_sem(PGconn *c, int from, int to, double amt) {
    sem_wait(&g_sem); /* decrementa: 1→0; bloqueia se já estiver em zero */
    double bf = pg_read_balance(c, from);
    double bt = pg_read_balance(c, to);
    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
    sem_post(&g_sem); /* incrementa: 0→1; acorda a próxima thread */
}

/*
 * CENÁRIO 4 — SELECT FOR UPDATE (lock de linha no banco)
 *
 * O banco adquire locks de linha nas duas contas antes de qualquer leitura.
 * Outras transações que tentarem as mesmas linhas ficam bloqueadas até o
 * COMMIT. Como o lock é por linha (não global), transferências envolvendo
 * contas distintas ocorrem em paralelo sem interferência.
 *
 * Lock ordering (sempre bloqueia a conta de menor id primeiro): garante que
 * todas as transações adquirem locks na mesma ordem, eliminando deadlock
 * circular (Thread A esperando B, Thread B esperando A).
 */
static void do_forupdate(PGconn *c, int from, int to, double amt) {
    /* ordenação por id: evita deadlock entre transações concorrentes */
    int a = from < to ? from : to;
    int b = from < to ? to   : from;
    char sql[256];

    pg_exec_q(c, "BEGIN;");

    /* FOR UPDATE reserva as linhas atomicamente até o COMMIT */
    snprintf(sql, sizeof(sql),
             "SELECT id,balance FROM accounts"
             " WHERE id IN(%d,%d) ORDER BY id FOR UPDATE;", a, b);

    PGresult *r = PQexec(c, sql);
    __sync_fetch_and_add(&g_db_calls, 1);
    double bf = 0.0, bt = 0.0;
    /* extrai saldos das linhas retornadas, associando cada id à sua variável */
    for (int i = 0; i < PQntuples(r); i++) {
        int    id  = atoi(PQgetvalue(r, i, 0));
        double bal = atof(PQgetvalue(r, i, 1));
        if (id == from) bf = bal; else bt = bal;
    }
    PQclear(r);

    pg_write_balance(c, from, bf - amt);
    pg_write_balance(c, to,   bt + amt);
    pg_exec_q(c, "COMMIT;"); /* libera os locks de linha */
}

/*
 * CENÁRIO 5 — SERIALIZABLE + retry
 *
 * O PostgreSQL monitora dependências entre transações concorrentes. Se
 * executá-las em paralelo produziria um resultado inconsistente com
 * qualquer execução serial, ele aborta uma delas (SQLSTATE 40001).
 * A aplicação detecta o erro verificando PQtransactionStatus e reinicia
 * a transação do zero com os valores atuais do banco.
 *
 * O número de retries mede a contenção: alta contenção → muitos retries
 * → overhead significativo.
 */
static void do_serializable(PGconn *c, int from, int to, double amt) {
    for (;;) {
        /* garante estado limpo antes de iniciar — pode haver ROLLBACK pendente
           de uma iteração anterior */
        if (PQtransactionStatus(c) != PQTRANS_IDLE)
            pg_exec_q(c, "ROLLBACK;");

        pg_exec_q(c, "BEGIN ISOLATION LEVEL SERIALIZABLE;");

        double bf = pg_read_balance(c, from);
        double bt = pg_read_balance(c, to);

        /* o banco pode ter abortado a transação durante o SELECT */
        if (PQtransactionStatus(c) == PQTRANS_INERROR) {
            __sync_fetch_and_add(&g_retries, 1); continue;
        }

        pg_write_balance(c, from, bf - amt);
        pg_write_balance(c, to,   bt + amt);

        /* o banco pode ter abortado a transação durante os UPDATEs */
        if (PQtransactionStatus(c) == PQTRANS_INERROR) {
            __sync_fetch_and_add(&g_retries, 1); continue;
        }

        /* COMMIT retorna false se o banco detectar falha de serialização */
        if (pg_exec_q(c, "COMMIT;")) break;
        __sync_fetch_and_add(&g_retries, 1);
    }
}

/*
 * CENÁRIO 6 — UPDATE atômico (melhor solução)
 *
 * Elimina completamente o read-modify-write da aplicação: o cálculo é feito
 * inteiramente dentro do banco com "SET balance = balance + Δ". Não existe
 * valor saindo para a memória da thread — não há janela de corrida.
 *
 * Lock ordering (menor id primeiro): as duas linhas são atualizadas sempre
 * na mesma ordem por todas as threads, impedindo deadlock circular entre
 * transações que atualizariam as mesmas contas em ordem inversa.
 *
 * Resultado: menor número de queries (2 por transferência vs 4 dos outros
 * modos), locks de linha de duração mínima e escala linear com threads.
 */
static void do_atomic(PGconn *c, int from, int to, double amt) {
    /* determina a ordem de atualização baseada no id para garantir lock ordering */
    int    first    = from < to ? from : to;
    int    second   = from < to ? to   : from;
    double d_first  = (first  == from) ? -amt : +amt;
    double d_second = (second == from) ? -amt : +amt;
    char sql[128];

    for (;;) {
        if (PQtransactionStatus(c) != PQTRANS_IDLE)
            pg_exec_q(c, "ROLLBACK;");

        pg_exec_q(c, "BEGIN;");

        /* o banco executa leitura + cálculo + escrita atomicamente — sem janela de corrida */
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

/* ── Despachante ─────────────────────────────────────────────── */

/* Executa a transferência usando o mecanismo correspondente ao modo. */
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
