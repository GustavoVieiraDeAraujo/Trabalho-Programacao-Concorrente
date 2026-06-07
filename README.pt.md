# Race Conditions em Transações Bancárias

## O que é

Programa em C que simula transferências bancárias concorrentes para demonstrar e medir o impacto de race conditions. Ao final da execução gera um **relatório PDF** completo com resultados, análises e comparações. Seis mecanismos de sincronização são testados em sequência, cada um com 500 transferências paralelas entre 10 contas bancárias:

| # | Mecanismo | Nível |
|---|-----------|-------|
| 1 | Sem controle | — |
| 2 | `pthread_mutex_t` | Aplicação |
| 3 | `sem_t` (semáforo POSIX) | Aplicação |
| 4 | `SELECT FOR UPDATE` | Banco de dados |
| 5 | `SERIALIZABLE` + retry | Banco de dados |
| 6 | `UPDATE` atômico | Banco de dados |

---

## Requisitos

| Dependência | Versão | Instalação |
|-------------|--------|-----------|
| GCC | ≥ 9 | `sudo apt install build-essential` |
| libpq-dev | qualquer | `sudo apt install libpq-dev` |
| Docker + Compose | qualquer | [docs.docker.com/engine/install](https://docs.docker.com/engine/install) |
| Python 3 + weasyprint | ≥ 3.8 | `pip install weasyprint` |

---

## Como executar

**Passo 1 — instalar dependências (Ubuntu/Debian, executar uma vez):**

```bash
make setup
```

Instala GCC, libpq-dev, Docker, Docker Compose e weasyprint automaticamente.
Se o Docker acabou de ser instalado, execute `newgrp docker` antes de continuar.

**Passo 2 — executar:**

```bash
# Execução completa — verifica deps, compila, sobe o banco, roda, gera PDFs
make start

# Limpa tudo (resultados, binários, container)
make clean
```

O `make start` faz tudo em sequência:
1. Verifica se todas as dependências estão instaladas (encerra com mensagem clara se não estiverem)
2. Apaga resultados e binários anteriores
3. Compila o projeto
4. Sobe o PostgreSQL via Docker Compose (porta 5433)
5. Executa os 6 cenários + teste de escalabilidade
6. Converte o HTML gerado em PDF com weasyprint
7. Derruba o container e apaga os binários

O relatório é salvo em `results/report_YYYYMMDD_HHMMSS.pdf`.

> `results/`, `bin/` e `build/` estão no `.gitignore` — são gerados em tempo de execução e não fazem parte do repositório.

---

## Arquitetura

O programa usa o padrão **produtor-consumidor** com uma fila de trabalho circular compartilhada entre threads.

![Diagrama de arquitetura](docs/architecture.svg)

**Fluxo de um cenário:**

1. `db_reset()` — zera os saldos e garante estado limpo
2. O produtor gera N transferências aleatórias e empurra na fila
3. N workers consomem a fila em paralelo, cada um com sua própria conexão ao banco
4. Ao esvaziar a fila, todos os workers encerram
5. `db_total()` — soma todos os saldos e compara com o valor inicial
6. Qualquer diferença indica race condition

---

## Estrutura do projeto

```
.
├── include/
│   ├── database.h    — constantes do banco e interface de conexão/queries
│   ├── queue.h       — fila circular com pthread_mutex_t + pthread_cond_t
│   ├── transfer.h    — enum Mode e assinatura de transfer_exec()
│   ├── result.h      — struct Result (discrepância, tempo, db_calls, retries)
│   └── report.h      — API de geração do relatório
├── src/
│   ├── main.c        — orquestração: ProducerArgs, workers, run_scenario()
│   ├── database.c    — pg_connect(), db_reset(), db_total(), pg_read_balance(), pg_write_balance()
│   ├── queue.c       — wq_push(), wq_pop(), wq_done() com sincronização
│   ├── transfer.c    — 6 implementações de transfer_exec(), uma por Mode
│   └── report.c      — lê templates/, substitui placeholders, escreve HTML
├── templates/
│   ├── report.html   — template do relatório com placeholders {{CHAVE}}
│   └── style.css     — estilos ABNT (A4, Arial 12pt, entrelinhas 1,5)
├── docs/
│   ├── architecture.svg          — diagrama da arquitetura
│   └── project_specification.pdf — especificação original do trabalho
├── scripts/
│   └── setup.sh                  — instalador de dependências
├── results/          — PDFs gerados (criado automaticamente)
├── docker-compose.yml
└── Makefile
```

---

## Como o relatório é gerado

O relatório não usa biblioteca de PDF — o processo tem três etapas:

1. **C gera HTML**: `report.c` lê `templates/report.html`, substitui cada `{{PLACEHOLDER}}` pelos dados reais da execução (tabelas, métricas, badges) e salva em `results/report_*.html`

2. **Python converte para PDF**: o Makefile chama weasyprint via Python para converter o HTML em PDF.

3. **HTML é apagado**: apenas o PDF final fica em `results/`

Os placeholders disponíveis no template:

| Placeholder | Conteúdo |
|-------------|----------|
| `{{CSS}}` | Conteúdo de `style.css` embutido no HTML |
| `{{GENERATED_AT}}` | Data e hora da execução |
| `{{CFG_ROWS}}` | Linhas da tabela de configuração |
| `{{SCN_N_DYNAMIC}}` | Callout de resultado + métricas do cenário N |
| `{{SCN_N_BADGES}}` | Badges de status e nível do cenário N |
| `{{SUMMARY_ROWS}}` | Linhas da tabela comparativa |
| `{{SCALING_ROWS}}` | Linhas da tabela de escalabilidade |
| `{{BEST_CALLOUT}}` | Callout com o mecanismo de melhor desempenho |

---

## Configuração do banco

Definida em `include/database.h` e `docker-compose.yml`:

| Parâmetro | Valor |
|-----------|-------|
| Imagem | `postgres:16-alpine` |
| Porta | `5433` (evita conflito com instâncias locais na 5432) |
| Usuário / Senha | `banco` / `senha123` |
| Database | `banco` |
| Contas | 10 (tabela `accounts`) |
| Saldo inicial | R$ 1.000,00 por conta |

---

🇺🇸 Looking for the English version? See [README.en.md](README.en.md)
