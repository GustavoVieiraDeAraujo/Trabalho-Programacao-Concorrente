#!/usr/bin/env bash
# setup.sh — installs all dependencies needed to run this project
# Tested on Ubuntu 22.04 / 24.04

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
ok()   { echo -e "${GREEN}[OK]${NC} $*"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }
fail() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }
step() { echo -e "\n${YELLOW}▶ $*${NC}"; }

# ── 1. OS check ─────────────────────────────────────────────────
step "Checking operating system..."
if ! command -v apt-get &>/dev/null; then
    fail "This script requires a Debian/Ubuntu-based system (apt-get not found)."
fi
ok "Compatible system."

# ── 2. GCC + build tools ────────────────────────────────────────
step "Installing GCC and build tools..."
sudo apt-get update -qq
sudo apt-get install -y build-essential
ok "GCC $(gcc --version | head -1 | grep -oP '\d+\.\d+\.\d+' | head -1) installed."

# ── 3. libpq-dev ────────────────────────────────────────────────
step "Installing libpq-dev (PostgreSQL C client)..."
sudo apt-get install -y libpq-dev
ok "libpq-dev installed."

# ── 4. Docker ───────────────────────────────────────────────────
step "Checking Docker..."
if command -v docker &>/dev/null; then
    ok "Docker already installed: $(docker --version)"
else
    warn "Docker not found — installing..."
    sudo apt-get install -y ca-certificates curl gnupg lsb-release
    sudo install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
        | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
        | sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
    sudo apt-get update -qq
    sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
    ok "Docker installed."
fi

# ── 5. Docker Compose (plugin v2) ───────────────────────────────
step "Checking Docker Compose..."
if docker compose version &>/dev/null; then
    ok "Docker Compose available: $(docker compose version --short)"
else
    fail "Docker Compose plugin not found. Reinstall Docker Engine."
fi

# ── 6. Docker group permission ──────────────────────────────────
step "Checking Docker group permission..."
if groups "$USER" | grep -q docker; then
    ok "User '$USER' is already in the docker group."
else
    warn "Adding '$USER' to the docker group..."
    sudo usermod -aG docker "$USER"
    warn "Run 'newgrp docker' or log out and back in before running 'make start'."
fi

# ── 7. Python 3 + weasyprint ────────────────────────────────────
step "Installing Python 3 and weasyprint..."
sudo apt-get install -y python3 python3-pip

# System libraries required for weasyprint font rendering
sudo apt-get install -y \
    libpango-1.0-0 libpangoft2-1.0-0 libpangocairo-1.0-0 \
    libcairo2 libgdk-pixbuf2.0-0 libffi-dev \
    fonts-liberation 2>/dev/null || true

# Ubuntu 24.04+ requires --break-system-packages for pip installs outside a venv
if pip3 install --quiet weasyprint 2>/dev/null || \
   pip3 install --quiet --break-system-packages weasyprint 2>/dev/null; then
    python3 -c "from weasyprint import HTML" 2>/dev/null \
        && ok "weasyprint installed." \
        || fail "weasyprint installed but cannot be imported. Try: pip3 install --break-system-packages weasyprint"
else
    fail "Failed to install weasyprint. Try manually: pip3 install --break-system-packages weasyprint"
fi

# ── 8. Final summary ────────────────────────────────────────────
echo ""
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
echo -e "${GREEN}  All dependencies installed. Run:  make start${NC}"
echo -e "${GREEN}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
