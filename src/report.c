/*
 * report.c
 *
 * Geração do relatório HTML substituindo tokens {{PLACEHOLDER}} nos templates
 * templates/report.html (português) ou templates/report.en.html (inglês)
 * pelos dados reais coletados durante o experimento.
 *
 * Estratégia de geração:
 *   - Cada seção dinâmica é acumulada em um buffer de memória (open_memstream).
 *   - report_close() lê o template inteiro, substitui cada {{CHAVE}} pelo
 *     conteúdo do buffer correspondente e salva o HTML final.
 *   - O Makefile então chama weasyprint para converter HTML → PDF.
 *
 * O código C é responsável apenas pelos números e tabelas dinâmicas.
 * Toda a estrutura HTML estática, textos e CSS vivem nos templates.
 */

#define _GNU_SOURCE
#include "report.h"
#include "database.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>

/* Idioma ativo para este relatório */
static Lang g_lang;

/* Seleciona entre português e inglês em tempo de execução */
#define S(pt, en) (g_lang == LANG_PT ? (pt) : (en))

/* ── Buffers em memória para cada placeholder ────────────────── */

static FILE  *g_config_fp;           char *g_config_buf;           size_t g_config_sz;
static FILE  *g_scenario_fp[6];      char *g_scenario_buf[6];      size_t g_scenario_sz[6];
static FILE  *g_badges_fp[6];        char *g_badges_buf[6];        size_t g_badges_sz[6];
static FILE  *g_summary_fp;          char *g_summary_buf;          size_t g_summary_sz;
static FILE  *g_scaling_fp;          char *g_scaling_buf;          size_t g_scaling_sz;
static FILE  *g_best_fp;             char *g_best_buf;             size_t g_best_sz;

static char g_generated_at[64]; /* data/hora formatada para o relatório */
static char g_css_buf[65536];   /* conteúdo do style.css embutido no HTML */

/* Stream ativo no momento: as funções de escrita usam g_cur como destino */
static FILE *g_cur = NULL;
static char  g_output_path[80]; /* caminho do arquivo HTML de saída */

/* ── Escrita no stream ativo ─────────────────────────────────── */

/* Wrapper de fprintf usando o stream g_cur como destino atual */
static void w(const char *fmt, ...) {
    if (!g_cur) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_cur, fmt, ap);
    va_end(ap);
}

/* ── Substituição de placeholders ───────────────────────────── */

/*
 * Substitui todas as ocorrências de 'key' em 'src' por 'val'.
 * Retorna uma nova alocação com o resultado e libera 'src'.
 * O chamador não precisa gerenciar memória intermediária: basta encadear
 * chamadas e liberar o ponteiro final.
 */
static char *replace_placeholder(char *src, const char *key, const char *val) {
    if (!val) val = "";
    size_t key_len = strlen(key);
    size_t val_len = strlen(val);

    /* conta ocorrências para calcular o tamanho exato do buffer de saída */
    int count = 0;
    const char *p = src;
    while ((p = strstr(p, key))) { count++; p += key_len; }
    if (!count) return src; /* nenhuma ocorrência: retorna src sem alterar */

    size_t src_len = strlen(src);
    char *dst = malloc(src_len + count * (val_len - key_len) + 1);
    char *out = dst;
    p = src;
    const char *hit;
    while ((hit = strstr(p, key))) {
        size_t prefix = hit - p;
        memcpy(out, p, prefix); out += prefix;
        memcpy(out, val, val_len); out += val_len;
        p = hit + key_len;
    }
    strcpy(out, p); /* copia o restante após a última ocorrência */
    free(src);
    return dst;
}

/* ── Leitura de arquivo para heap ────────────────────────────── */

/* Lê o conteúdo completo de um arquivo e retorna como string alocada no heap.
   Retorna NULL se o arquivo não for encontrado. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "Erro: arquivo nao encontrado: %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!fread(buf, 1, sz, f)) { free(buf); fclose(f); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ── Geração de badges (etiquetas de status) ─────────────────── */

/* Retorna o HTML do badge de corretude: verde (correto) ou vermelho (corrompido) */
static const char *ok_badge(int ok) {
    if (ok)
        return g_lang == LANG_PT
            ? "<span class=\"badge badge-ok\">&#x2705; CORRETO</span>"
            : "<span class=\"badge badge-ok\">&#x2705; CORRECT</span>";
    return g_lang == LANG_PT
        ? "<span class=\"badge badge-fail\">&#x274C; CORROMPIDO</span>"
        : "<span class=\"badge badge-fail\">&#x274C; CORRUPTED</span>";
}

/* Retorna o rótulo do nível de controle (Nenhum / Aplicação / Banco) */
static const char *level_badge(Mode mode) {
    static const char *lbl_pt[] = { "Nenhum", "Aplicação", "Aplicação", "Banco", "Banco", "Banco" };
    static const char *lbl_en[] = { "None", "Application", "Application", "Database", "Database", "Database" };
    return (g_lang == LANG_PT ? lbl_pt : lbl_en)[mode];
}

/* Retorna a classe CSS do badge de nível: vermelho para ausência de controle,
   azul para aplicação, verde para banco */
static const char *level_css(Mode mode) {
    return (mode <= APP_SEM) ? "badge-level" : "badge-ok";
}

/* ── Seção dinâmica de cada cenário (callout + cards de métricas) ─ */

/*
 * Gera o bloco HTML dinâmico de um cenário: callout de resultado (verde se
 * correto, vermelho se corrompido) seguido de cards com as métricas numéricas
 * (discrepância, TPS, tempo, chamadas ao BD, retries).
 */
static void write_scenario_dynamic(FILE *fp, Mode mode, const Result *r) {
    int    ok  = (r->discrepancy > -0.01 && r->discrepancy < 0.01);
    double tps = r->n_transfers / (r->ms / 1000.0);
    FILE  *prev = g_cur;
    g_cur = fp;

    if (ok) {
        w("<div class=\"callout callout-success\"><strong>%s</strong>",
          S("Saldo Preservado — Sem Race Condition",
            "Balance Preserved — No Race Condition"));
        if (g_lang == LANG_PT)
            w("Todas as <b>%d transferências</b> foram contabilizadas corretamente. "
              "Saldo: R$&nbsp;%.2f → R$&nbsp;%.2f.",
              r->n_transfers, r->initial, r->final);
        else
            w("All <b>%d transfers</b> were correctly accounted for. "
              "Balance: R$&nbsp;%.2f → R$&nbsp;%.2f.",
              r->n_transfers, r->initial, r->final);
        w("</div>\n");
    } else {
        w("<div class=\"callout callout-danger\"><strong>%s</strong>",
          S("Saldo Corrompido — Race Condition Detectada",
            "Balance Corrupted — Race Condition Detected"));
        const char *gone = r->discrepancy < 0
            ? S("desapareceram do sistema", "disappeared from the system")
            : S("foram criados do nada",    "were created from nothing");
        if (g_lang == LANG_PT)
            w("Após %d transferências: R$&nbsp;%.2f → R$&nbsp;%.2f "
              "&nbsp;<b>(Δ = R$&nbsp;%+.2f)</b>. "
              "Em um banco real, R$&nbsp;%.2f %s.",
              r->n_transfers, r->initial, r->final, r->discrepancy,
              r->discrepancy < 0 ? -r->discrepancy : r->discrepancy, gone);
        else
            w("After %d transfers: R$&nbsp;%.2f → R$&nbsp;%.2f "
              "&nbsp;<b>(Δ = R$&nbsp;%+.2f)</b>. "
              "In a real bank, R$&nbsp;%.2f %s.",
              r->n_transfers, r->initial, r->final, r->discrepancy,
              r->discrepancy < 0 ? -r->discrepancy : r->discrepancy, gone);
        w("</div>\n");
    }

    /* cards de métricas: discrepância, TPS, tempo, chamadas ao BD e retries */
    w("<div class=\"metrics\">\n");
    w("<div class=\"metric\"><div class=\"metric-val %s\">R$ %+.2f</div>"
      "<div class=\"metric-lbl\">%s</div></div>\n",
      ok ? "ok" : "fail", r->discrepancy,
      S("Discrepância", "Discrepancy"));
    w("<div class=\"metric\"><div class=\"metric-val\">%.0f</div>"
      "<div class=\"metric-lbl\">%s</div></div>\n",
      tps, S("Transf/s", "Trans/s"));
    w("<div class=\"metric\"><div class=\"metric-val\">%.0f ms</div>"
      "<div class=\"metric-lbl\">%s</div></div>\n",
      r->ms, S("Tempo total", "Total time"));
    w("<div class=\"metric\"><div class=\"metric-val\">%d</div>"
      "<div class=\"metric-lbl\">%s</div></div>\n",
      r->db_calls, S("Chamadas ao BD", "DB Calls"));
    /* retries só são relevantes no modo SERIALIZABLE */
    if (mode == DB_SERIALIZABLE)
        w("<div class=\"metric\"><div class=\"metric-val %s\">%d</div>"
          "<div class=\"metric-lbl\">Retries</div></div>\n",
          r->retries > 0 ? "fail" : "ok", r->retries);
    w("</div>\n");

    g_cur = prev;
}

/* Gera os badges de corretude e nível de controle para o cabeçalho do cenário */
static void write_scenario_badges(FILE *fp, Mode mode, const Result *r) {
    int ok = (r->discrepancy > -0.01 && r->discrepancy < 0.01);
    FILE *prev = g_cur;
    g_cur = fp;
    w("%s\n", ok_badge(ok));
    w("<span class=\"badge %s\">%s</span>\n", level_css(mode), level_badge(mode));
    g_cur = prev;
}

/* ═══════════════════════════════════════════════════════════════
 * API pública (chamada em sequência pelo main)
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Inicializa os buffers de memória para cada placeholder, define o idioma
 * e lê o CSS do template para embutir inline no HTML.
 * Deve ser a primeira função chamada antes de qualquer report_*().
 */
void report_open(Lang lang) {
    g_lang = lang;

    /* configura locale para formatação de datas no idioma correto */
    if (lang == LANG_PT)
        setlocale(LC_TIME, "pt_BR.UTF-8");
    else
        setlocale(LC_TIME, "en_US.UTF-8");

    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    /* monta o caminho do arquivo de saída com timestamp para evitar sobrescrita */
    const char *fname_fmt = (lang == LANG_PT)
        ? "results/report_%Y%m%d_%H%M%S.pt.html"
        : "results/report_%Y%m%d_%H%M%S.en.html";
    strftime(g_output_path, sizeof(g_output_path), fname_fmt, t);

    /* formata a data de geração para exibição no relatório */
    const char *date_fmt = (lang == LANG_PT)
        ? "%d de %B de %Y, %H:%M"
        : "%B %d, %Y, %H:%M";
    strftime(g_generated_at, sizeof(g_generated_at), date_fmt, t);

    /* cria os buffers de memória (open_memstream: arquivo em RAM) */
    g_config_fp  = open_memstream(&g_config_buf,  &g_config_sz);
    g_summary_fp = open_memstream(&g_summary_buf, &g_summary_sz);
    g_scaling_fp = open_memstream(&g_scaling_buf, &g_scaling_sz);
    g_best_fp    = open_memstream(&g_best_buf,    &g_best_sz);
    for (int i = 0; i < 6; i++) {
        g_scenario_fp[i] = open_memstream(&g_scenario_buf[i], &g_scenario_sz[i]);
        g_badges_fp[i]   = open_memstream(&g_badges_buf[i],   &g_badges_sz[i]);
    }

    /* lê o CSS e armazena para embutir inline: evita dependência de arquivo externo no PDF */
    char *css = read_file("templates/style.css");
    if (css) {
        strncpy(g_css_buf, css, sizeof(g_css_buf) - 1);
        free(css);
    }

    printf("Relatorio (%s): %s\n", lang == LANG_PT ? "PT" : "EN", g_output_path);
}

/* Gera as linhas da tabela de configuração ({{CFG_ROWS}}) */
void report_header(int threads, int transfers) {
    g_cur = g_config_fp;

    if (g_lang == LANG_PT) {
        w("<tr><td>Contas bancárias</td>"
          "<td class=\"center\"><b>%d</b></td>"
          "<td>Contas na tabela <code>accounts</code></td></tr>\n", NUM_ACCTS);
        w("<tr><td>Saldo inicial</td>"
          "<td class=\"center\"><b>R$ %.2f</b></td>"
          "<td>Por conta — total R$&nbsp;%.2f</td></tr>\n",
          INIT_BAL, NUM_ACCTS * INIT_BAL);
        w("<tr><td>Threads workers</td>"
          "<td class=\"center\"><b>%d</b></td>"
          "<td>Processam transferências em paralelo</td></tr>\n", threads);
        w("<tr><td>Transferências</td>"
          "<td class=\"center\"><b>%d</b></td>"
          "<td>Por cenário</td></tr>\n", transfers);
        w("<tr><td>Fila de trabalho</td>"
          "<td class=\"center\"><b>pthread_cond_t</b></td>"
          "<td>Padrão produtor-consumidor com buffer limitado</td></tr>\n");
    } else {
        w("<tr><td>Bank accounts</td>"
          "<td class=\"center\"><b>%d</b></td>"
          "<td>Rows in the <code>accounts</code> table</td></tr>\n", NUM_ACCTS);
        w("<tr><td>Initial balance</td>"
          "<td class=\"center\"><b>R$ %.2f</b></td>"
          "<td>Per account — total R$&nbsp;%.2f</td></tr>\n",
          INIT_BAL, NUM_ACCTS * INIT_BAL);
        w("<tr><td>Worker threads</td>"
          "<td class=\"center\"><b>%d</b></td>"
          "<td>Process transfers in parallel</td></tr>\n", threads);
        w("<tr><td>Transfers</td>"
          "<td class=\"center\"><b>%d</b></td>"
          "<td>Per scenario</td></tr>\n", transfers);
        w("<tr><td>Work queue</td>"
          "<td class=\"center\"><b>pthread_cond_t</b></td>"
          "<td>Producer-consumer pattern with bounded buffer</td></tr>\n");
    }

    g_cur = NULL;
}

/* Delega a geração da seção dinâmica e dos badges do cenário 'num' */
void report_scenario(int num, Mode mode, const Result *r) {
    int idx = num - 1;
    write_scenario_dynamic(g_scenario_fp[idx], mode, r);
    write_scenario_badges(g_badges_fp[idx], mode, r);
}

/*
 * Gera as linhas da tabela comparativa ({{SUMMARY_ROWS}}) e o callout
 * do melhor resultado ({{BEST_CALLOUT}}).
 * O mecanismo campeão é o de maior TPS entre os que produziram dados corretos.
 */
void report_summary(const Result res[], int n) {
    double best_tps = 0; int best_idx = -1;
    for (int i = 0; i < n; i++) {
        int    ok  = (res[i].discrepancy > -0.01 && res[i].discrepancy < 0.01);
        double tps = res[i].n_transfers / (res[i].ms / 1000.0);
        if (ok && tps > best_tps) { best_tps = tps; best_idx = i; }
    }

    g_cur = g_summary_fp;

    const char **titles  = (g_lang == LANG_PT) ? SCENARIO_TITLE : SCENARIO_TITLE_EN;
    const char  *lbl_none = S("Nenhum", "None");
    const char  *lbl_app  = S("Aplicação", "Application");
    const char  *lbl_db   = S("Banco", "Database");

    const char *level_lbl[] = { lbl_none, lbl_app, lbl_app, lbl_db, lbl_db, lbl_db };
    static const char *level_c[] = {
        "badge-fail","badge-level","badge-level","badge-ok","badge-ok","badge-ok"
    };

    /* ranking visual: as três primeiras linhas corretas recebem classes CSS de destaque */
    int rank = 0;
    static const char *rank_css[] = {"rank-1","rank-2","rank-3","","",""};

    for (int i = 0; i < n; i++) {
        int    ok  = (res[i].discrepancy > -0.01 && res[i].discrepancy < 0.01);
        double tps = res[i].n_transfers / (res[i].ms / 1000.0);
        const char *rc     = (ok && rank < 3) ? rank_css[rank++] : "";
        const char *trophy = (i == best_idx) ? " 🏆" : "";
        w("<tr class=\"%s\">"
          "<td>%s</td>"
          "<td class=\"center\"><span class=\"badge %s\">%s</span></td>"
          "<td class=\"center %s\">%s</td>"
          "<td class=\"right\"><b>%.0f/s</b>%s</td>"
          "<td class=\"right td-muted\">%.0f ms</td>"
          "<td class=\"right td-muted\">%d</td></tr>\n",
          rc,
          titles[i],
          level_c[i], level_lbl[i],
          ok ? "td-ok" : "td-fail",
          ok ? "&#x2705;" : "&#x274C;",
          tps, trophy,
          res[i].ms, res[i].retries);
    }
    g_cur = NULL;

    /* callout do campeão (mecanismo correto de maior TPS) */
    if (best_idx >= 0) {
        g_cur = g_best_fp;
        w("<div class=\"callout callout-success\">"
          "<strong>%s</strong>"
          "%s &mdash; <b>%.0f&nbsp;%s</b></div>\n",
          S("&#x1F3C6; Melhor resultado com dados corretos",
            "&#x1F3C6; Best result with correct data"),
          titles[best_idx], best_tps,
          S("transferências/s", "transfers/s"));
        g_cur = NULL;
    }
}

/*
 * Gera as linhas da tabela de escalabilidade ({{SCALING_ROWS}}).
 * Células com dados corrompidos recebem indicador visual (⚠️).
 */
void report_scaling(const Mode modes[], int nm,
                    const int threads[] __attribute__((unused)), int nt,
                    double tps[][4], int ok[][4]) {
    const char **titles = (g_lang == LANG_PT) ? SCENARIO_TITLE : SCENARIO_TITLE_EN;
    g_cur = g_scaling_fp;
    for (int i = 0; i < nm; i++) {
        w("<tr><td>%s</td>", titles[modes[i]]);
        for (int j = 0; j < nt; j++) {
            if (ok[i][j])
                w("<td class=\"center\">%.0f/s</td>", tps[i][j]);
            else
                /* ⚠️ indica que o saldo ficou corrompido nesta configuração */
                w("<td class=\"center td-fail\">%.0f/s &#x26A0;</td>", tps[i][j]);
        }
        w("</tr>\n");
    }
    g_cur = NULL;
}

/* Reservado para conclusão adicional; o texto principal está no template. */
void report_conclusion(const Result res[], int n) {
    (void)res; (void)n;
}

/*
 * Fecha todos os buffers de memória, lê o template HTML, substitui cada
 * {{PLACEHOLDER}} pelo buffer correspondente e salva o arquivo de saída.
 * Libera toda a memória alocada.
 */
void report_close(void) {
    /* fecha os streams: o conteúdo dos buffers só fica disponível após fclose */
    fclose(g_config_fp);
    fclose(g_summary_fp);
    fclose(g_scaling_fp);
    fclose(g_best_fp);
    for (int i = 0; i < 6; i++) {
        fclose(g_scenario_fp[i]);
        fclose(g_badges_fp[i]);
    }

    /* seleciona o template no idioma correto */
    const char *tmpl = (g_lang == LANG_PT)
        ? "templates/report.html"
        : "templates/report.en.html";

    char *html = read_file(tmpl);
    if (!html) return;

    /* substitui todos os placeholders pelos dados coletados */
    html = replace_placeholder(html, "{{CSS}}",           g_css_buf);
    html = replace_placeholder(html, "{{GENERATED_AT}}",  g_generated_at);
    html = replace_placeholder(html, "{{CFG_ROWS}}",      g_config_buf);
    html = replace_placeholder(html, "{{SCN_1_DYNAMIC}}", g_scenario_buf[0]);
    html = replace_placeholder(html, "{{SCN_2_DYNAMIC}}", g_scenario_buf[1]);
    html = replace_placeholder(html, "{{SCN_3_DYNAMIC}}", g_scenario_buf[2]);
    html = replace_placeholder(html, "{{SCN_4_DYNAMIC}}", g_scenario_buf[3]);
    html = replace_placeholder(html, "{{SCN_5_DYNAMIC}}", g_scenario_buf[4]);
    html = replace_placeholder(html, "{{SCN_6_DYNAMIC}}", g_scenario_buf[5]);
    html = replace_placeholder(html, "{{SCN_1_BADGES}}",  g_badges_buf[0]);
    html = replace_placeholder(html, "{{SCN_2_BADGES}}",  g_badges_buf[1]);
    html = replace_placeholder(html, "{{SCN_3_BADGES}}",  g_badges_buf[2]);
    html = replace_placeholder(html, "{{SCN_4_BADGES}}",  g_badges_buf[3]);
    html = replace_placeholder(html, "{{SCN_5_BADGES}}",  g_badges_buf[4]);
    html = replace_placeholder(html, "{{SCN_6_BADGES}}",  g_badges_buf[5]);
    html = replace_placeholder(html, "{{SUMMARY_ROWS}}",  g_summary_buf);
    html = replace_placeholder(html, "{{SCALING_ROWS}}",  g_scaling_buf);
    html = replace_placeholder(html, "{{BEST_CALLOUT}}",  g_best_buf);

    FILE *out = fopen(g_output_path, "w");
    if (out) { fputs(html, out); fclose(out); }

    /* libera todos os buffers alocados */
    free(html);
    free(g_config_buf); free(g_summary_buf); free(g_scaling_buf); free(g_best_buf);
    for (int i = 0; i < 6; i++) {
        free(g_scenario_buf[i]);
        free(g_badges_buf[i]);
    }
}
