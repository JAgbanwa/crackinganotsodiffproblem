#!/bin/bash
# deploy_vps.sh  --  One-command deploy to a fresh Ubuntu 22.04 VPS
#
# Run this ON the VPS:
#   curl -fsSL https://raw.githubusercontent.com/JAgbanwa/crackinganotsodiffproblem/main/deploy_vps.sh | bash
#
# Or copy locally and run:
#   scp deploy_vps.sh user@YOUR-VPS:~ && ssh user@YOUR-VPS 'bash deploy_vps.sh'
#
# Prerequisites:
#   - Ubuntu 22.04 LTS (fresh install)
#   - Port 80 open in firewall
#   - DNS A record pointing to this server's IP (optional but recommended)

set -euo pipefail

VPS_IP=$(curl -s https://api.ipify.org || hostname -I | awk '{print $1}')
REPO="https://github.com/JAgbanwa/crackinganotsodiffproblem.git"
DEPLOY_DIR="/opt/s3ceq"

echo "=========================================="
echo " S3CEQ Charity Engine BOINC Server Deploy"
echo " VPS IP: ${VPS_IP}"
echo "=========================================="

# ── 1. Install Docker & Docker Compose ────────────────────────────────────────
if ! command -v docker &>/dev/null; then
    echo "[1] Installing Docker..."
    apt-get update -q
    apt-get install -y --no-install-recommends ca-certificates curl gnupg
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
        | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
    chmod a+r /etc/apt/keyrings/docker.gpg
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" \
        > /etc/apt/sources.list.d/docker.list
    apt-get update -q
    apt-get install -y docker-ce docker-ce-cli containerd.io docker-compose-plugin
    systemctl enable docker
    echo "[1] Docker installed."
else
    echo "[1] Docker already installed."
fi

# ── 2. Clone / update the project repo ────────────────────────────────────────
if [ -d "${DEPLOY_DIR}/.git" ]; then
    echo "[2] Updating repo..."
    git -C "${DEPLOY_DIR}" pull --ff-only
else
    echo "[2] Cloning repo..."
    git clone "${REPO}" "${DEPLOY_DIR}"
fi
cd "${DEPLOY_DIR}"

# ── 3. Create .env from template if not present ────────────────────────────────
if [ ! -f .env ]; then
    echo "[3] Creating .env (edit passwords before production!)..."
    cp .env.example .env
    # Auto-fill VPS IP
    sed -i "s|YOUR-VPS-IP-OR-DOMAIN|${VPS_IP}|g" .env
    echo ""
    echo "  *** EDIT ${DEPLOY_DIR}/.env to set strong passwords before continuing! ***"
    echo "  *** Press Enter to continue with default (insecure) values, or Ctrl+C to abort ***"
    read -r _
else
    echo "[3] .env already exists, skipping."
fi

# ── 4. Open firewall ──────────────────────────────────────────────────────────
echo "[4] Configuring firewall (ufw)..."
ufw allow 80/tcp  2>/dev/null || true
ufw allow 22/tcp  2>/dev/null || true
ufw --force enable 2>/dev/null || true

# ── 5. Build and start containers ─────────────────────────────────────────────
echo "[5] Building and starting BOINC server..."
docker compose pull mysql
docker compose build boinc
docker compose up -d

echo ""
echo "=========================================="
echo " Deployment complete!"
echo " BOINC project URL: http://${VPS_IP}/s3ceq/"
echo ""
echo " Monitor:  docker compose logs -f boinc"
echo " Solutions: docker exec s3ceq-boinc-1 cat /srv/s3ceq/solutions_master.txt"
echo ""
echo " NEXT STEPS:"
echo "  1. Apply for Charity Engine project approval:"
echo "     https://www.charityengine.com/apply-for-computing-power/"
echo "     (mention project URL and GitHub: ${REPO})"
echo "  2. While waiting, your BOINC server is already live and accepts"
echo "     standard BOINC clients. Share the URL with volunteers."
echo "=========================================="
