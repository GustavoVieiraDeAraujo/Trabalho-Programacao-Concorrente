#pragma once

typedef struct {
    double initial, final, discrepancy;
    int    n_transfers, db_calls, retries;
    double ms;
} Result;
