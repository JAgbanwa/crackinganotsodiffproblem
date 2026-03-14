#!/bin/bash
# setup_boinc_project.sh  —  Bootstraps a Charity Engine / BOINC project
# for searching solutions to:
#     Y^2 = (m + 6n)^2 + (36n^3 - 19) / m   (m ≠ 0, m | 36n^3-19)
#
# Prerequisites:
#   • BOINC server tools installed (boinc-server-maker or manual install)
#   • Running as user 'boincadm' (or adapt BOINC_USER below)
#   • Access to a MySQL/MariaDB instance
#
# Usage:
#   bash setup_boinc_project.sh

set -euo pipefail

# ── Configuration ─────────────────────────────────────────────────────────────
PROJECT_NAME="s3ceq"
PROJECT_LONG="Equation Search: Y^2=(m+6n)^2+(36n^3-19)/m"
APP_NAME="s3ceq_worker"
BOINC_USER="boincadm"
BOINC_HOME="/home/${BOINC_USER}"
PROJECT_DIR="${BOINC_HOME}/projects/${PROJECT_NAME}"
BOINC_SRC_DIR="/usr/local/boinc"        # adjust if needed

echo "================================================================"
echo " Setting up BOINC project: ${PROJECT_NAME}"
echo "================================================================"

# ── 1. Create project skeleton ────────────────────────────────────────────────
if [ ! -d "${PROJECT_DIR}" ]; then
    echo "[1] Creating project directory..."
    make_project --url_base "http://$(hostname -f)" \
                 --db_user "${BOINC_USER}" \
                 --project_root "${PROJECT_DIR}" \
                 "${PROJECT_NAME}" "${PROJECT_LONG}"
else
    echo "[1] Project directory already exists — skipping make_project"
fi

# ── 2. Register the application ───────────────────────────────────────────────
echo "[2] Registering app '${APP_NAME}'..."
cd "${PROJECT_DIR}"

cat > db/apps.sql << EOF
INSERT IGNORE INTO app (name, user_friendly_name, deprecated) VALUES
  ('${APP_NAME}', 'S3C Equation Worker', 0);
EOF
mysql -u "${BOINC_USER}" "${PROJECT_NAME}" < db/apps.sql

# ── 3. Compile C worker and deploy ───────────────────────────────────────────
echo "[3] Building C worker..."
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${SCRIPT_DIR}"
make all

# Create app version directory
APP_VERSION_DIR="${PROJECT_DIR}/apps/${APP_NAME}/1.00/x86_64-pc-linux-gnu__sse2"
mkdir -p "${APP_VERSION_DIR}"

cp worker_s3ceq "${APP_VERSION_DIR}/worker_s3ceq"

cat > "${APP_VERSION_DIR}/version.xml" << 'EOF'
<version>
  <file>
    <name>worker_s3ceq</name>
    <main_program/>
  </file>
</version>
EOF

cd "${PROJECT_DIR}"
bin/xadd
bin/update_versions

# ── 4. Copy templates ─────────────────────────────────────────────────────────
echo "[4] Installing templates..."
cp "${SCRIPT_DIR}/templates/job.xml"    "${PROJECT_DIR}/templates/${APP_NAME}_in"
cp "${SCRIPT_DIR}/templates/result.xml" "${PROJECT_DIR}/templates/${APP_NAME}_out"

# ── 5. Install daemons in config.xml ─────────────────────────────────────────
echo "[5] Patching config.xml with daemons..."

CONFIG="${PROJECT_DIR}/config.xml"

# Insert before </daemons> if not already present
if ! grep -q "work_generator" "${CONFIG}"; then
    sed -i "s|</daemons>|    <daemon>\n        <cmd>python3 ${SCRIPT_DIR}/work_generator.py</cmd>\n        <output>work_generator.log</output>\n    </daemon>\n    <daemon>\n        <cmd>python3 ${SCRIPT_DIR}/assimilator.py</cmd>\n        <output>assimilator.log</output>\n    </daemon>\n</daemons>|" "${CONFIG}"
fi

# ── 6. Start project ──────────────────────────────────────────────────────────
echo "[6] Starting BOINC daemons..."
cd "${PROJECT_DIR}"
bin/start

echo ""
echo "================================================================"
echo " Setup complete! Project is running."
echo " Project directory: ${PROJECT_DIR}"
echo " Monitor: tail -f ${PROJECT_DIR}/log_*/\*.log"
echo "================================================================"
