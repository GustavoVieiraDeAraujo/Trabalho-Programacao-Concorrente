# Race Conditions in Bank Transfers

Project for the **Concurrent Programming** course at the Department of Computer Science, University of Brasilia ([specification](./docs/project_specification.pdf)). A C program that simulates concurrent bank transfers to demonstrate and measure the impact of race conditions, testing 6 synchronization mechanisms with 500 parallel transfers between 10 accounts. Generates a PDF report automatically.

🇧🇷 Versao em portugues: [README.md](README.md)

---

## Table of Contents

- [Participants](#participants)
- [Technologies](#technologies)
- [Project Scope](#project-scope)
- [Project Structure](#project-structure)
- [Requirements](#requirements)
- [Demo Video](#demo-video)
- [How to Run](#how-to-run)
- [Architecture](#architecture)
- [Synchronization Mechanisms](#synchronization-mechanisms)
- [Report Generation](#report-generation)
- [Database Configuration](#database-configuration)

---

## Participants

| Name                              | Student ID |
|-----------------------------------|------------|
| Gustavo Vieira de Araujo          | 211068440  |

---

## Technologies

| Technology         | Usage                                                    |
|--------------------|----------------------------------------------------------|
| C (GCC >= 9)       | Main language — threads, mutex, semaphores                |
| PostgreSQL 16      | Database for DB-level synchronization scenarios            |
| libpq              | PostgreSQL driver for C                                   |
| Docker + Compose   | PostgreSQL containerization                               |
| Python 3           | HTML to PDF conversion via weasyprint                     |
| Pthreads           | Thread creation and synchronization                       |

---

## Project Scope

| Requirement                                        | Implementation                                                     |
|----------------------------------------------------|---------------------------------------------------------------------|
| Demonstrate race conditions                        | 500 parallel transfers between 10 accounts with no synchronization  |
| Mutex (application level)                          | `pthread_mutex_t` protecting balance read and write                 |
| Semaphore (application level)                      | `sem_t` (POSIX semaphore) as an alternative to mutex                |
| SELECT FOR UPDATE (database level)                 | Pessimistic row-level lock in PostgreSQL                            |
| SERIALIZABLE + retry (database level)              | Maximum isolation with automatic serialization conflict retry       |
| Atomic UPDATE (database level)                     | `UPDATE SET balance = balance + X` with no prior read               |
| Automatic report                                   | PDF generated with results, metrics and cross-scenario comparisons  |

---

## Project Structure

| Directory / File                         | Description                                                       |
|------------------------------------------|-------------------------------------------------------------------|
| `src/`                                   | C source code                                                      |
| `src/main.c`                             | Orchestration: ProducerArgs, workers, run_scenario()               |
| `src/database.c`                         | Connection, reset, balance read and write on PostgreSQL             |
| `src/queue.c`                            | Circular queue with `pthread_mutex_t` + `pthread_cond_t`           |
| `src/transfer.c`                         | 6 implementations of `transfer_exec()`, one per mechanism          |
| `src/report.c`                           | Template reading, placeholder substitution, HTML generation        |
| `include/`                               | Headers                                                            |
| `include/database.h`                     | DB constants and connection/query interface                        |
| `include/queue.h`                        | Thread-safe circular queue                                         |
| `include/transfer.h`                     | Mode enum and `transfer_exec()` signature                          |
| `include/result.h`                       | Result struct (discrepancy, time, db_calls, retries)               |
| `include/report.h`                       | Report generation API                                              |
| `templates/`                             | HTML and CSS report templates                                      |
| `templates/report.html`                  | PT-BR template with `{{PLACEHOLDER}}` tokens                      |
| `templates/report.en.html`               | EN template with `{{PLACEHOLDER}}` tokens                         |
| `templates/style.css`                    | ABNT formatting (A4, Arial 12pt, 1.5 line spacing)                |
| `docs/`                                  | Documentation                                                      |
| `docs/architecture.svg`                  | Producer-consumer architecture diagram                             |
| `docs/project_specification.pdf`         | Original assignment specification                                  |
| `docs/relatorio_entrega.pdf`             | Delivery report                                                    |
| `docs/how_to_run.mp4`                    | Project demo video                                                 |
| `scripts/setup.sh`                       | Dependency installer                                               |
| `docker-compose.yml`                     | Containerized PostgreSQL configuration                             |
| `Makefile`                               | Build, run, clean and setup                                        |

---

## Requirements

| Dependency           | Version  | Installation                                                      |
|----------------------|----------|-------------------------------------------------------------------|
| GCC                  | >= 9     | `sudo apt install build-essential`                                |
| libpq-dev            | any      | `sudo apt install libpq-dev`                                      |
| Docker + Compose     | any      | [docs.docker.com/engine/install](https://docs.docker.com/engine/install) |
| Python 3 + weasyprint| >= 3.8   | `pip install weasyprint`                                          |

Or install everything at once:

```bash
make setup
```

---

## Demo Video

The file [`docs/how_to_run.mp4`](docs/how_to_run.mp4) shows the project running end-to-end: dependency installation, compilation, database startup, scenario execution, and PDF generation.

---

## How to Run

```bash
# Install dependencies (once)
make setup

# Full run (checks deps, compiles, starts DB, runs scenarios, generates PDF)
make start

# Clean everything (results, binaries, container)
make clean
```

`make start` runs in sequence:

1. Verifies all dependencies are installed
2. Removes previous results and binaries
3. Compiles the project
4. Starts PostgreSQL via Docker Compose (port 5433)
5. Runs all 6 scenarios + scalability test
6. Converts the generated HTML to PDF with weasyprint
7. Stops the container and removes binaries

The report is saved to `results/report_YYYYMMDD_HHMMSS.pdf`.

> `results/`, `bin/`, and `build/` are in `.gitignore` — they are created at runtime.

---

## Architecture

The program uses the **producer-consumer** pattern with a shared circular work queue between threads:

![Architecture diagram](docs/architecture.svg)

**Scenario flow:**

```
1. db_reset()            → zeroes all balances, guarantees clean state
2. Producer              → generates N random transfers and pushes onto queue
3. N workers in parallel → consume the queue, each with its own DB connection
4. Queue empty           → all workers exit
5. db_total()            → sums balances and compares with initial total
6. Difference != 0       → race condition detected
```

---

## Synchronization Mechanisms

| # | Mechanism | Level | Description |
|---|-----------|-------|-------------|
| 1 | No control | — | Unprotected read and write — demonstrates the problem |
| 2 | `pthread_mutex_t` | Application | Global mutex protects the critical section (read + write) |
| 3 | `sem_t` (POSIX semaphore) | Application | Binary semaphore as an alternative to mutex |
| 4 | `SELECT FOR UPDATE` | Database | Pessimistic row-level lock — locks the account during transaction |
| 5 | `SERIALIZABLE` + retry | Database | Maximum isolation with automatic retry on serialization conflict |
| 6 | Atomic `UPDATE` | Database | `UPDATE SET balance = balance + X` with no prior read — atomic operation |

---

## Report Generation

The report uses no PDF library — the pipeline has three steps:

```
1. C generates HTML  → report.c reads templates/report.html, replaces {{PLACEHOLDER}} with live data
2. Python converts   → weasyprint transforms the HTML into PDF
3. HTML is deleted   → only the final PDF remains in results/
```

Available placeholders:

| Placeholder | Content |
|---|---|
| `{{CSS}}` | Inlined `style.css` content |
| `{{GENERATED_AT}}` | Report generation date and time |
| `{{CFG_ROWS}}` | Configuration table rows |
| `{{SCN_N_DYNAMIC}}` | Result callout + metrics for scenario N |
| `{{SCN_N_BADGES}}` | Status and level badges for scenario N |
| `{{SUMMARY_ROWS}}` | Comparative summary table rows |
| `{{SCALING_ROWS}}` | Scalability table rows |
| `{{BEST_CALLOUT}}` | Callout highlighting the best-performing mechanism |

---

## Database Configuration

Defined in `include/database.h` and `docker-compose.yml`:

| Parameter      | Value                     |
|----------------|---------------------------|
| Image          | `postgres:16-alpine`      |
| Port           | `5433`                    |
| User / Password| `banco` / `banco`         |
| Database       | `banco`                   |
| Accounts       | 10 (table `accounts`)     |
| Initial balance| R$ 1,000.00 per account   |
