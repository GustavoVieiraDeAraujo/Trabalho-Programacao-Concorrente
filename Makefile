CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread -I/usr/include/postgresql -Iinclude
LDFLAGS = -lpq -lpthread

SRCS = src/main.c src/database.c src/queue.c src/transfer.c src/report.c
BIN  = bin/bank

# ── Install all dependencies (run once before 'make start') ────
setup:
	@bash scripts/setup.sh

# ── Check required tools before running ────────────────────────
check:
	@echo "Checking dependencies..."
	@command -v gcc        >/dev/null 2>&1 || { echo "[ERROR] gcc not found.        Run: make setup"; exit 1; }
	@command -v docker     >/dev/null 2>&1 || { echo "[ERROR] docker not found.     Run: make setup"; exit 1; }
	@docker compose version>/dev/null 2>&1 || { echo "[ERROR] docker compose not found. Run: make setup"; exit 1; }
	@command -v python3    >/dev/null 2>&1 || { echo "[ERROR] python3 not found.    Run: make setup"; exit 1; }
	@python3 -c "from weasyprint import HTML" 2>/dev/null || { echo "[ERROR] weasyprint not found. Run: make setup"; exit 1; }
	@pkg-config --exists libpq 2>/dev/null || \
	    ls /usr/include/postgresql/libpq-fe.h >/dev/null 2>&1 || \
	    { echo "[ERROR] libpq-dev not found. Run: make setup"; exit 1; }
	@echo "All dependencies OK."

# ── Full run ────────────────────────────────────────────────────
# 1. Check dependencies
# 2. Remove previous results and binaries
# 3. Compile
# 4. Start PostgreSQL
# 5. Execute
# 6. Convert HTML reports to PDF
# 7. Stop PostgreSQL and remove binaries (keeps results/)
start: check
	@rm -rf results build bin
	@mkdir -p results build bin
	$(CC) $(CFLAGS) -o $(BIN) $(SRCS) $(LDFLAGS)
	docker compose up -d
	@echo "Waiting for PostgreSQL to start..."
	@sleep 4
	./$(BIN)
	@python3 -c "\
import glob, os; \
from weasyprint import HTML; \
files = glob.glob('results/*.html'); \
[HTML(filename=f).write_pdf(f.replace('.html','.pdf')) or os.remove(f) or print('PDF generated:', f.replace('.html','.pdf')) for f in files]"
	@rm -rf build bin
	@docker compose down -v --remove-orphans 2>/dev/null || true

# ── Full cleanup ────────────────────────────────────────────────
clean:
	@rm -rf results build bin
	@docker compose down -v --remove-orphans 2>/dev/null || true
	@echo "Clean."

.PHONY: setup check start clean
