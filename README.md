# Race Conditions em Transacoes Bancarias

Projeto da disciplina **Programacao Concorrente** do Departamento de Ciencia da Computacao da Universidade de Brasilia ([especificacao](./docs/project_specification.pdf)). Programa em C que simula transferencias bancarias concorrentes para demonstrar e medir o impacto de race conditions, testando 6 mecanismos de sincronizacao com 500 transferencias paralelas entre 10 contas. Gera relatorio PDF automaticamente.

🇺🇸 English version: [README.en.md](README.en.md)

---

## Sumario

- [Participantes](#participantes)
- [Tecnologias](#tecnologias)
- [Escopo do Projeto](#escopo-do-projeto)
- [Estrutura do Projeto](#estrutura-do-projeto)
- [Requisitos](#requisitos)
- [Demonstracao em Video](#demonstracao-em-video)
- [Como Executar](#como-executar)
- [Arquitetura](#arquitetura)
- [Mecanismos de Sincronizacao](#mecanismos-de-sincronizacao)
- [Geracao do Relatorio](#geracao-do-relatorio)
- [Configuracao do Banco](#configuracao-do-banco)

---

## Participantes

| Nome                              | Matricula |
|-----------------------------------|-----------|
| Gustavo Vieira de Araujo          | 211068440 |

---

## Tecnologias

| Tecnologia         | Uso                                                      |
|--------------------|----------------------------------------------------------|
| C (GCC >= 9)       | Linguagem principal — threads, mutex, semaforos           |
| PostgreSQL 16      | Banco de dados para os cenarios de sincronizacao em BD    |
| libpq              | Driver PostgreSQL para C                                  |
| Docker + Compose   | Containerizacao do PostgreSQL                             |
| Python 3           | Conversao HTML para PDF via weasyprint                    |
| Pthreads           | Criacao e sincronizacao de threads                        |

---

## Escopo do Projeto

| Requisito                                          | Implementacao                                                      |
|----------------------------------------------------|--------------------------------------------------------------------|
| Demonstrar race conditions                         | 500 transferencias paralelas entre 10 contas sem sincronizacao      |
| Mutex (nivel de aplicacao)                         | `pthread_mutex_t` protegendo leitura e escrita de saldo             |
| Semaforo (nivel de aplicacao)                      | `sem_t` (semaforo POSIX) como alternativa ao mutex                  |
| SELECT FOR UPDATE (nivel de banco)                 | Lock pessimista por linha no PostgreSQL                             |
| SERIALIZABLE + retry (nivel de banco)              | Isolamento maximo com tratamento de conflitos de serializacao       |
| UPDATE atomico (nivel de banco)                    | `UPDATE SET saldo = saldo + X` sem leitura previa                  |
| Relatorio automatico                               | PDF gerado com resultados, metricas e comparacoes entre cenarios    |

---

## Estrutura do Projeto

| Diretorio / Arquivo                      | Descricao                                                         |
|------------------------------------------|-------------------------------------------------------------------|
| `src/`                                   | Codigo fonte C                                                     |
| `src/main.c`                             | Orquestracao: ProducerArgs, workers, run_scenario()                |
| `src/database.c`                         | Conexao, reset, leitura e escrita de saldos no PostgreSQL          |
| `src/queue.c`                            | Fila circular com `pthread_mutex_t` + `pthread_cond_t`             |
| `src/transfer.c`                         | 6 implementacoes de `transfer_exec()`, uma por mecanismo           |
| `src/report.c`                           | Leitura de templates, substituicao de placeholders, geracao HTML    |
| `include/`                               | Headers                                                            |
| `include/database.h`                     | Constantes do banco e interface de conexao/queries                  |
| `include/queue.h`                        | Fila circular thread-safe                                          |
| `include/transfer.h`                     | Enum Mode e assinatura de `transfer_exec()`                        |
| `include/result.h`                       | Struct Result (discrepancia, tempo, db_calls, retries)             |
| `include/report.h`                       | API de geracao do relatorio                                        |
| `templates/`                             | Templates HTML e CSS do relatorio                                  |
| `templates/report.html`                  | Template PT-BR com placeholders `{{CHAVE}}`                        |
| `templates/report.en.html`               | Template EN com placeholders `{{CHAVE}}`                           |
| `templates/style.css`                    | Estilos ABNT (A4, Arial 12pt, entrelinhas 1,5)                    |
| `docs/`                                  | Documentacao                                                       |
| `docs/architecture.svg`                  | Diagrama da arquitetura produtor-consumidor                        |
| `docs/project_specification.pdf`         | Especificacao original do trabalho                                  |
| `docs/relatorio_entrega.pdf`             | Relatorio de entrega                                               |
| `docs/how_to_run.mp4`                    | Video de demonstracao do projeto                                   |
| `scripts/setup.sh`                       | Instalador de dependencias                                         |
| `docker-compose.yml`                     | Configuracao do PostgreSQL containerizado                          |
| `Makefile`                               | Build, execucao, limpeza e setup                                    |

---

## Requisitos

| Dependencia          | Versao   | Instalacao                                                        |
|----------------------|----------|-------------------------------------------------------------------|
| GCC                  | >= 9     | `sudo apt install build-essential`                                |
| libpq-dev            | qualquer | `sudo apt install libpq-dev`                                      |
| Docker + Compose     | qualquer | [docs.docker.com/engine/install](https://docs.docker.com/engine/install) |
| Python 3 + weasyprint| >= 3.8   | `pip install weasyprint`                                          |

Ou instale tudo de uma vez:

```bash
make setup
```

---

## Demonstracao em Video

O arquivo [`docs/how_to_run.mp4`](docs/how_to_run.mp4) mostra o projeto sendo executado do inicio ao fim: instalacao das dependencias, compilacao, subida do banco, execucao dos cenarios e geracao do PDF.

---

## Como Executar

```bash
# Instalar dependencias (uma vez)
make setup

# Execucao completa (verifica deps, compila, sobe banco, roda cenarios, gera PDF)
make start

# Limpar tudo (resultados, binarios, container)
make clean
```

O `make start` executa em sequencia:

1. Verifica dependencias instaladas
2. Apaga resultados e binarios anteriores
3. Compila o projeto
4. Sobe o PostgreSQL via Docker Compose (porta 5433)
5. Executa os 6 cenarios + teste de escalabilidade
6. Converte o HTML gerado em PDF com weasyprint
7. Derruba o container e apaga os binarios

O relatorio e salvo em `results/report_YYYYMMDD_HHMMSS.pdf`.

> `results/`, `bin/` e `build/` estao no `.gitignore` — sao gerados em tempo de execucao.

---

## Arquitetura

O programa usa o padrao **produtor-consumidor** com uma fila de trabalho circular compartilhada entre threads:

![Diagrama de arquitetura](docs/architecture.svg)

**Fluxo de um cenario:**

```
1. db_reset()           → zera saldos e garante estado limpo
2. Produtor             → gera N transferencias aleatorias e empurra na fila
3. N workers em paralelo → consomem a fila, cada um com sua conexao ao banco
4. Fila vazia           → todos os workers encerram
5. db_total()           → soma saldos e compara com valor inicial
6. Diferenca != 0       → race condition detectada
```

---

## Mecanismos de Sincronizacao

| # | Mecanismo | Nivel | Descricao |
|---|-----------|-------|-----------|
| 1 | Sem controle | — | Leitura e escrita sem protecao — demonstra o problema |
| 2 | `pthread_mutex_t` | Aplicacao | Mutex global protege a secao critica (leitura + escrita) |
| 3 | `sem_t` (semaforo POSIX) | Aplicacao | Semaforo binario como alternativa ao mutex |
| 4 | `SELECT FOR UPDATE` | Banco de dados | Lock pessimista por linha — trava a conta durante a transacao |
| 5 | `SERIALIZABLE` + retry | Banco de dados | Isolamento maximo com retry automatico em caso de conflito |
| 6 | `UPDATE` atomico | Banco de dados | `UPDATE SET saldo = saldo + X` sem leitura previa — operacao atomica |

---

## Geracao do Relatorio

O relatorio nao usa biblioteca de PDF — o processo tem tres etapas:

```
1. C gera HTML     → report.c le templates/report.html, substitui {{PLACEHOLDER}} por dados reais
2. Python converte → weasyprint transforma o HTML em PDF
3. HTML e apagado  → apenas o PDF final fica em results/
```

Placeholders disponiveis no template:

| Placeholder | Conteudo |
|---|---|
| `{{CSS}}` | Conteudo de `style.css` embutido no HTML |
| `{{GENERATED_AT}}` | Data e hora da execucao |
| `{{CFG_ROWS}}` | Linhas da tabela de configuracao |
| `{{SCN_N_DYNAMIC}}` | Callout de resultado + metricas do cenario N |
| `{{SCN_N_BADGES}}` | Badges de status e nivel do cenario N |
| `{{SUMMARY_ROWS}}` | Linhas da tabela comparativa |
| `{{SCALING_ROWS}}` | Linhas da tabela de escalabilidade |
| `{{BEST_CALLOUT}}` | Callout com o mecanismo de melhor desempenho |

---

## Configuracao do Banco

Definida em `include/database.h` e `docker-compose.yml`:

| Parametro      | Valor                     |
|----------------|---------------------------|
| Imagem         | `postgres:16-alpine`      |
| Porta          | `5433`                    |
| Usuario/Senha  | `banco` / `banco`         |
| Database       | `banco`                   |
| Contas         | 10 (tabela `accounts`)    |
| Saldo inicial  | R$ 1.000,00 por conta     |
