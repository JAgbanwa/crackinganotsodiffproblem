# Dockerfile  —  BOINC server for s3ceq (Charity Engine / BOINC project)
#
# Builds a complete BOINC server image ready to accept CE volunteer clients.
# Requires a MySQL container (see docker-compose.yml).
#
# Environment variables expected at runtime:
#   URL_BASE          e.g. http://your-vps-domain.com  (no trailing slash)
#   DB_HOST           mysql  (or whatever the compose service is named)
#   DB_NAME           s3ceq
#   DB_USER           boincadm
#   DB_PASS           changeme
#   PROJECT_NAME      s3ceq  (must match URL structure)

FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PROJECT_NAME=s3ceq
ENV BOINC_USER=boincadm
ENV PROJECT_DIR=/home/boincadm/projects/s3ceq

# ── System packages ────────────────────────────────────────────────────────────
RUN apt-get update && apt-get install -y --no-install-recommends \
    apache2 \
    libapache2-mod-php \
    php \
    php-mysql \
    php-xml \
    python3 \
    python3-pip \
    mysql-client \
    gcc \
    g++ \
    make \
    libssl-dev \
    zlib1g-dev \
    libmysqlclient-dev \
    libcurl4-openssl-dev \
    m4 \
    autoconf \
    automake \
    libtool \
    pkg-config \
    curl \
    git \
    sudo \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# ── Create boincadm user ───────────────────────────────────────────────────────
RUN useradd -m -s /bin/bash boincadm && \
    echo "boincadm ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

# ── Install BOINC server from source ──────────────────────────────────────────
WORKDIR /tmp
RUN git clone --depth=1 https://github.com/BOINC/boinc.git boinc-src && \
    cd boinc-src && \
    ./_autosetup && \
    ./configure --disable-client --disable-manager --enable-server \
                --disable-fcgi && \
    make -j$(nproc) && \
    make install && \
    cd /tmp && rm -rf boinc-src

# ── Python packages for our daemons ───────────────────────────────────────────
RUN pip3 install --no-cache-dir mysql-connector-python requests

# ── Copy project files ─────────────────────────────────────────────────────────
WORKDIR /srv/s3ceq
COPY worker.c         .
COPY work_generator.py .
COPY assimilator.py   .
COPY validator.py     .
COPY templates/       templates/

# ── Compile the worker binary for Linux x86_64 ───────────────────────────────
RUN gcc -O3 -march=x86-64 -Wall -Wextra -std=c11 -lm -static \
        -o worker_s3ceq worker.c && \
    strip worker_s3ceq && \
    echo "Worker built: $(ls -lh worker_s3ceq)"

# ── Copy entrypoint ────────────────────────────────────────────────────────────
COPY docker-entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# ── Apache configuration ───────────────────────────────────────────────────────
RUN a2enmod cgi rewrite headers && \
    echo "ServerName localhost" >> /etc/apache2/apache2.conf

EXPOSE 80

ENTRYPOINT ["/entrypoint.sh"]
