#pragma once
#include "result.h"
#include "transfer.h"

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
