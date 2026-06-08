/*
 * report.h
 *
 * API de geração do relatório HTML/PDF.
 *
 * As funções devem ser chamadas nesta ordem:
 *   report_open()      — inicializa buffers e define o idioma
 *   report_header()    — escreve a tabela de configuração do experimento
 *   report_scenario()  — chamada N_MODES vezes, uma por cenário
 *   report_summary()   — escreve a tabela comparativa e o callout do campeão
 *   report_scaling()   — escreve a tabela de escalabilidade (modo × threads)
 *   report_conclusion()— reservado (conteúdo estático está no template)
 *   report_close()     — substitui os placeholders e salva o arquivo HTML
 *
 * O Makefile converte o HTML gerado para PDF via weasyprint e apaga o HTML.
 */

#pragma once
#include "result.h"
#include "transfer.h"

/* Idioma do relatório a ser gerado */
typedef enum { LANG_PT = 0, LANG_EN = 1 } Lang;

void report_open(Lang lang);
void report_header(int threads, int transfers);
void report_scenario(int num, Mode mode, const Result *r);
void report_summary(const Result res[], int n);
void report_scaling(const Mode modes[], int nm,
                    const int threads[], int nt,
                    double tps[][4], int ok[][4]);
void report_conclusion(const Result res[], int n);
void report_close(void);
