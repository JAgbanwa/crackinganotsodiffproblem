# crackinganotsodiffproblem

Search for all integer solutions to Equation 7.10 from
[this Figshare preprint](https://figshare.com/articles/preprint/Closed_form_formulas_on_the_sums_of_three_cubes_for_k_114_192_/30509981),
run on [Charity Engine](https://www.charityengine.com/) (BOINC distributed computing).

**Ultimate goal:** Crack the open problem $x^3 + y^3 + z^3 = 114$.  
Every integer solution to the equation below directly yields such a triple.  
114 is the smallest number not yet known to be representable as a sum of three cubes.

---

## The Equation

$$Y^2 = (m + 6n)^2 + \frac{36n^3 - 19}{m}, \quad m \neq 0, \; m \mid 36n^3-19$$

For each integer $n$, let $\text{val} = 36n^3 - 19$.  
We enumerate all integer divisors $m$ of $\text{val}$ and check whether
$(m+6n)^2 + \text{val}/m$ is a perfect square.

---

## Algorithm (v3 — 128-bit Pollard-rho)

| Step | Method | Cost |
|------|--------|------|
| Factorise \|val\| ≤ 2⁶⁴ | 64-bit Pollard-rho + Miller-Rabin | $O(\text{val}^{1/4})$ |
| Factorise \|val\| > 2⁶⁴ | **128-bit** Pollard-rho + Miller-Rabin | $O(\text{val}^{1/4})$ |
| Enumerate divisors | from prime factorisation | $O(d(\text{val}))$ |
| Check perfect square | Newton isqrt on `__int128` | $O(\log \text{val})$ |

**v3 vs v2:**  
- v2 had a hard 64-bit ceiling: fast factorisation only up to $|n| \leq 787{,}000$  
- v3 uses full 128-bit Pollard-rho, extending the fast path to $|n| \leq 2.1 \times 10^{12}$  
- For $|n|$ up to 787k: **~26,000 n/sec** per core (same as v2 — 64-bit path preserved)  
- For $|n|$ = 800k–2.1T: **~830 n/sec** per core (128-bit mulmod via binary method)

**Search ceiling with CE (500k cores):**

| Range | Time at 500k cores |
|-------|--------------------|
| $\|n\| \leq 787{,}000$ (v2 ceiling) | < 1 second |
| $\|n\| \leq 2.1 \times 10^{12}$ (v3 ceiling) | ~2.5 hours |

---

## Running Locally

```bash
# Build the native binary
make all

# Search n = [0, 1,000,000] (~38 seconds on a single core, v3 fast path)
echo "0 1000000" > wu.txt && time ./worker_s3ceq wu.txt out.txt

# Search beyond the old 64-bit ceiling (n = 800k–810k) — uses 128-bit Pollard-rho
echo "800000 810000" > wu.txt && time ./worker_s3ceq wu.txt out.txt

# Parallel search (forever, 4 workers, auto-resumable)
python3 fast_local_search.py --workers 4
```

---

## Deploying to Charity Engine (BOINC)

### Overview

Charity Engine is a BOINC-based platform with ~500,000 volunteer CPUs.
Once approved as a project, your work units run on those machines and
results flow back to your BOINC server automatically.

### Step 1 — Apply to Charity Engine

Submit a project application at:  
**https://www.charityengine.com/apply-for-computing-power/**

In your application:
- Attach a link to this GitHub repo
- Describe the equation and scientific goal (finding integer solutions)
- State that your BOINC server URL will be `http://YOUR-VPS-IP/s3ceq/`

Approval typically takes a few days to several weeks.

### Step 2 — Provision a VPS

| Resource | Minimum |
|----------|---------|
| OS | Ubuntu 22.04 LTS |
| CPU | 2 vCPU |
| RAM | 4 GB |
| Disk | 20 GB SSD |
| Port | 80 open |

Any provider works: Hetzner (~€5/mo), DigitalOcean, Vultr, Linode, AWS EC2.

### Step 3 — One-command Deploy

SSH into the VPS, then:

```bash
curl -fsSL https://raw.githubusercontent.com/JAgbanwa/crackinganotsodiffproblem/main/deploy_vps.sh | sudo bash
```

This installs Docker, clones the repo, builds the BOINC server container,
and starts everything. Your project will be live at `http://YOUR-VPS-IP/s3ceq/`.

### Step 4 — (Optional) Point a Domain

Add an A record for e.g. `s3ceq.yourdomain.com` → VPS IP, then edit `/opt/s3ceq/.env`:

```
URL_BASE=http://s3ceq.yourdomain.com
```

Restart: `docker compose -f /opt/s3ceq/docker-compose.yml up -d`

### Step 5 — How Work Units Flow

```
Work Generator → BOINC Server → Charity Engine Volunteers
      ↑                                     |
   40k-WU                             run worker_s3ceq
      |                                     |
 Checkpoint                          return results
      |                                     ↓
  solutions_master.txt ← Assimilator ← Validator
```

- Each WU covers 50,000 consecutive $n$ values (~2 seconds per modern CPU)
- With 500k volunteers, this covers **25 billion $n$/day** 
- The work generator runs forever, expanding both positive and negative $n$

### Step 6 — Monitoring

```bash
# Follow logs on VPS
docker compose logs -f boinc

# Check solutions
docker exec s3ceq-boinc-1 cat /srv/s3ceq/solutions_master.txt

# BOINC web interface
http://YOUR-VPS-IP/s3ceq/
```

### Manual VPS Setup (without Docker)

Alternatively, run on a bare Ubuntu VPS:

```bash
# Install BOINC server (Ubuntu 22.04)
sudo apt-get install boinc-server-maker mysql-server apache2
sudo -u boincadm make_project --url_base http://YOUR-IP s3ceq "S3CEQ Search"

# Build and deploy worker
make all
make linux-docker   # builds apps/worker_s3ceq_linux (Linux static binary)

# Copy to BOINC app directory
mkdir -p ~/projects/s3ceq/apps/s3ceq_worker/1.00/x86_64-pc-linux-gnu__sse2/
cp apps/worker_s3ceq_linux ~/projects/s3ceq/apps/s3ceq_worker/1.00/x86_64-pc-linux-gnu__sse2/worker_s3ceq

# Start
bash setup_boinc_project.sh
```

---

## Repository Structure

| File | Purpose |
|------|---------|
| `worker.c` | C worker (v3: 128-bit Pollard-rho) — runs on volunteer machines |
| `fast_local_search.py` | Local parallel search using C worker |
| `work_generator.py` | BOINC work unit generator (runs on server) |
| `validator.py` | Algebraic result verifier (runs on server) |
| `assimilator.py` | Result deduplicator (runs on server) |
| `Dockerfile` | BOINC server container |
| `docker-compose.yml` | Full stack (BOINC server + MySQL) |
| `deploy_vps.sh` | One-command VPS deployment script |
| `Dockerfile.worker` | Builds Linux x86_64 static binary |
| `Makefile` | Build targets: `all`, `linux-docker`, `test`, `bench` |
| `templates/` | BOINC work-unit and result XML templates |
| `solutions_master.txt` | All solutions found (populated at runtime) |

---

## Search Progress

| Date | Range searched | Solutions | Worker version |
|------|---------------|-----------|----------------|
| 2026-03-14 | $n \in [-100{,}000,\, 100{,}000]$ | 0 | v2 (Pollard-rho, 64-bit) |
| 2026-03-14 | v3 deployed — ceiling extended to $\|n\| \leq 2.1 \times 10^{12}$ | — | v3 (128-bit Pollard-rho) |

The search is **ongoing** and expands continuously outward from $n = 0$.

### Version History

| Version | Key change | Speed |
|---------|-----------|-------|
| v1 | Trial division $O(\sqrt{\text{val}})$ | ~26 n/s/core |
| v2 | 64-bit Pollard-rho; ceiling $\|n\| \leq 787k$ | ~26,000 n/s/core |
| **v3** | **128-bit Pollard-rho; ceiling $\|n\| \leq 2.1 \times 10^{12}$** | **~26,000 n/s/core** (≤787k), **~830 n/s/core** (>787k) |

### Why 114?

$114 \equiv 6 \pmod{9}$ — not ruled out by the standard modular obstruction.  
It is the smallest positive integer whose status as a sum of three cubes remains unknown (as of 2026).  
The analogous problems for 33, 42, and 3 were all eventually solved — 33 and 42 in 2019 by Booker & Sutherland with solutions reaching $10^{15}$.  
Our parametric Equation 7.10 provides a targeted algebraic path specifically to 114.

---

*Powered by [Charity Engine](https://www.charityengine.com/) and [BOINC](https://boinc.berkeley.edu/).*
