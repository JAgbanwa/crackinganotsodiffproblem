#!/bin/bash
# docker-entrypoint.sh  —  Initialises and starts the BOINC project
set -euo pipefail

URL_BASE="${URL_BASE:-http://localhost}"
DB_HOST="${DB_HOST:-mysql}"
DB_NAME="${DB_NAME:-s3ceq}"
DB_USER="${DB_USER:-boincadm}"
DB_PASS="${DB_PASS:-changeme}"
PROJECT_NAME="${PROJECT_NAME:-s3ceq}"
PROJECT_DIR="/home/boincadm/projects/${PROJECT_NAME}"
APP_NAME="s3ceq_worker"

echo "=== S3CEQ BOINC Server Starting ==="
echo "URL_BASE=${URL_BASE}"
echo "PROJECT_DIR=${PROJECT_DIR}"

# ── Wait for MySQL to be ready ─────────────────────────────────────────────────
echo "[INIT] Waiting for MySQL at ${DB_HOST}..."
for i in $(seq 1 60); do
    if mysqladmin ping -h "${DB_HOST}" -u "${DB_USER}" -p"${DB_PASS}" --silent 2>/dev/null; then
        echo "[INIT] MySQL ready."
        break
    fi
    echo "[INIT] MySQL not ready yet (attempt $i/60)..."
    sleep 3
done

# ── Create BOINC project if not already initialised ───────────────────────────
if [ ! -f "${PROJECT_DIR}/config.xml" ]; then
    echo "[INIT] Creating BOINC project..."
    sudo -u boincadm make_project \
        --url_base "${URL_BASE}" \
        --db_host "${DB_HOST}" \
        --db_name "${DB_NAME}" \
        --db_user "${DB_USER}" \
        --db_passwd "${DB_PASS}" \
        --project_root "${PROJECT_DIR}" \
        "${PROJECT_NAME}" "S3C Equation Search"

    echo "[INIT] Registering application ${APP_NAME}..."
    mysql -h "${DB_HOST}" -u "${DB_USER}" -p"${DB_PASS}" "${DB_NAME}" << SQL
INSERT IGNORE INTO app (name, user_friendly_name, deprecated)
VALUES ('${APP_NAME}', 'S3C Equation Worker (Y^2=(m+6n)^2+(36n^3-19)/m)', 0);
SQL

    # ── Deploy worker binaries ─────────────────────────────────────────────────
    echo "[INIT] Deploying worker binaries..."

    # Linux x86_64
    LINUX_DIR="${PROJECT_DIR}/apps/${APP_NAME}/1.00/x86_64-pc-linux-gnu__sse2"
    mkdir -p "${LINUX_DIR}"
    cp /srv/s3ceq/worker_s3ceq "${LINUX_DIR}/worker_s3ceq"
    cat > "${LINUX_DIR}/version.xml" << 'EOF'
<version>
  <file>
    <name>worker_s3ceq</name>
    <main_program/>
  </file>
</version>
EOF

    # ── Install job/result templates ───────────────────────────────────────────
    cp /srv/s3ceq/templates/job.xml    "${PROJECT_DIR}/templates/${APP_NAME}_in"
    cp /srv/s3ceq/templates/result.xml "${PROJECT_DIR}/templates/${APP_NAME}_out"

    # ── Register app version with BOINC ────────────────────────────────────────
    cd "${PROJECT_DIR}" && sudo -u boincadm bin/xadd && sudo -u boincadm bin/update_versions

    # ── Patch config.xml to include our daemons ────────────────────────────────
    python3 /srv/s3ceq/patch_config.py "${PROJECT_DIR}/config.xml"

    echo "[INIT] Project initialisation complete."
else
    echo "[INIT] Project already initialised, skipping setup."
fi

# ── Configure Apache to serve the BOINC project ───────────────────────────────
cat > /etc/apache2/sites-available/boinc.conf << APACHECONF
<VirtualHost *:80>
    ServerName $(echo "${URL_BASE}" | sed 's|https\?://||')
    DocumentRoot ${PROJECT_DIR}/html
    Alias /s3ceq/cgi-bin/ ${PROJECT_DIR}/cgi-bin/
    Alias /s3ceq/download/ ${PROJECT_DIR}/download/
    Alias /s3ceq/upload/   ${PROJECT_DIR}/upload/

    <Directory ${PROJECT_DIR}/html>
        Options Indexes FollowSymLinks
        AllowOverride All
        Require all granted
    </Directory>
    <Directory ${PROJECT_DIR}/cgi-bin>
        Options ExecCGI
        AddHandler cgi-script .cgi
        Require all granted
    </Directory>
    <Directory ${PROJECT_DIR}/download>
        Options Indexes
        Require all granted
    </Directory>
    <Directory ${PROJECT_DIR}/upload>
        Require all granted
    </Directory>
    CustomLog \${APACHE_LOG_DIR}/boinc_access.log combined
    ErrorLog  \${APACHE_LOG_DIR}/boinc_error.log
</VirtualHost>
APACHECONF
a2dissite 000-default || true
a2ensite boinc
service apache2 start || apache2ctl start

# ── Start BOINC project daemons ────────────────────────────────────────────────
echo "[START] Starting BOINC daemons..."
cd "${PROJECT_DIR}"
sudo -u boincadm bin/start

# ── Start our work generator (as background daemon) ───────────────────────────
echo "[START] Starting work generator..."
sudo -u boincadm \
    BOINC_PROJECT_DIR="${PROJECT_DIR}" \
    python3 /srv/s3ceq/work_generator.py \
    >> "${PROJECT_DIR}/log_s3ceq/work_generator.log" 2>&1 &

# ── Start our assimilator ─────────────────────────────────────────────────────
echo "[START] Starting assimilator..."
sudo -u boincadm \
    BOINC_PROJECT_DIR="${PROJECT_DIR}" \
    python3 /srv/s3ceq/assimilator.py \
    >> "${PROJECT_DIR}/log_s3ceq/assimilator.log" 2>&1 &

echo ""
echo "=== BOINC Server is running ==="
echo "Project URL: ${URL_BASE}/${PROJECT_NAME}/"
echo "Logs: ${PROJECT_DIR}/log_*/"
echo ""

# ── Keep container alive by tailing logs ──────────────────────────────────────
tail -f "${PROJECT_DIR}/log_s3ceq/"*.log 2>/dev/null || \
tail -f /var/log/apache2/*.log
