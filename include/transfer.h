#pragma once
#include <libpq-fe.h>

typedef enum {
    UNSAFE = 0,
    APP_MUTEX,
    APP_SEM,
    DB_FORUPDATE,
    DB_SERIALIZABLE,
    DB_ATOMIC,
    N_MODES
} Mode;

extern const char *MODE_LABEL[N_MODES];
extern const char *SCENARIO_TITLE[N_MODES];
extern const char *SCENARIO_TITLE_EN[N_MODES];

void transfer_init(void);
void transfer_cleanup(void);
void transfer_exec(PGconn *c, Mode mode, int from, int to, double amt);
