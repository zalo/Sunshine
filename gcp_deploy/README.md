# Sunshine WebRTC Streaming - GCP Deployment

GPU-accelerated game streaming via Sunshine on Google Cloud Platform with automatic start/stop via Cloudflare Workers.

## Architecture

```
┌─────────────────┐     ┌──────────────────┐     ┌─────────────────────┐
│  User Browser   │────▶│ Cloudflare Worker │────▶│  GCP L4 GPU VM      │
│                 │     │ (start/stop VM)   │     │                     │
│  WebRTC Stream  │◀────│                   │◀────│  Docker Container   │
└─────────────────┘     └──────────────────┘     │  ├─ Xorg + NVIDIA    │
                               │                 │  ├─ XFCE Desktop     │
                               │                 │  ├─ Sunshine         │
                               ▼                 │  ├─ FastAPI Proxy    │
                       ┌──────────────────┐     │  └─ Cloudflare Tunnel│
                       │ Cloudflare Tunnel │◀───┴─────────────────────┘
                       │ (secure access)   │
                       └──────────────────┘
```

## Current Configuration (as of deployment)

| Component | Value |
|-----------|-------|
| GCP Project | `john-cli` |
| GCP Zone | `us-west1-a` (Oregon) |
| VM Instance Name | `sunshine-gpu` |
| Machine Type | `g2-standard-4` (4 vCPUs, 16GB RAM, 1x L4 GPU) |
| Cloudflare Worker | `sunshine-cloud-gaming.makeshifted.workers.dev` |
| Worker Custom Domains | `gaming.sels.tech`, `play.sels.tech` |
| Cloudflare Account | `ab77c04897c9dcc5f04b753c51dbe517` |
| KV Namespace | `611d2f05129443cc81d3f9e4f95e91a7` |
| Internal Tunnel Domain | `sunshine-stream.sels.tech` (embedded via iframe) |
| Tunnel Name | `gcp-gaming` (ID: `94ecba9a-99ef-4d73-88f5-491a98e85205`) |
| Service Account | `sunshine-worker@john-cli.iam.gserviceaccount.com` |

## Features

- **On-demand GPU VM**: Auto-starts when users visit the page, auto-shutdown after 5 min idle
- **Full GPU acceleration**: NVIDIA Xorg for desktop rendering + NVENC encoding
- **No port forwarding**: Cloudflare Tunnel provides secure access
- **Seamless UX**: Loading spinner while VM starts, iframe embeds stream when ready
- **Client heartbeat tracking**: VM stays alive while users have the page open
- **Low latency**: WebRTC streaming with STUN/TURN support
- **Cost effective**: Pay only for active usage (L4 GPU ~$0.53/hr)

## File Structure

```
gcp_deploy/
├── Dockerfile              # Docker image for Sunshine streaming server
├── docker-compose.yml      # Local development and building
├── create-vm.sh            # GCP VM creation script
├── start.sh                # Container startup script (supervisord helper)
├── supervisord.conf        # Process manager configuration
├── xorg.conf               # NVIDIA Xorg configuration for headless GPU
├── sunshine.conf           # Sunshine server configuration
├── proxy.py                # FastAPI reverse proxy for single-port access
├── README.md               # This file
└── cloudflare_worker/      # Cloudflare Worker for VM management
    ├── src/
    │   └── index.ts        # Worker code (start/stop VM, serve landing page)
    ├── package.json        # Node.js dependencies
    ├── tsconfig.json       # TypeScript configuration
    └── wrangler.toml       # Wrangler deployment configuration
```

## Prerequisites

1. **Google Cloud Account** with billing enabled
2. **Cloudflare Account** with a domain
3. **Local tools**:
   - Docker with NVIDIA Container Toolkit (for building)
   - gcloud CLI (authenticated)
   - wrangler CLI (`npm install -g wrangler`)
   - cloudflared CLI

## GPU Quota Requirement

**IMPORTANT**: Before creating the VM, you must have GPU quota in GCP.

To request GPU quota:
1. Go to: https://console.cloud.google.com/iam-admin/quotas?project=john-cli
2. Filter by: `GPUS_ALL_REGIONS` or search "GPUs (all regions)"
3. Select the quota and click **Edit Quotas**
4. Request increase to at least `1`
5. Fill in contact info and reason (e.g., "Cloud gaming development")

GPU quota requests are usually approved within 24-48 hours for new projects.

## Quick Start (for future setup)

### 1. Authenticate tools

```bash
# Google Cloud
gcloud auth login
gcloud config set project john-cli

# Cloudflare (wrangler)
wrangler login

# Verify
gcloud config list
wrangler whoami
```

### 2. Build and Push Docker Image

```bash
cd /home/selstad/Desktop/Sunshine

# Build Sunshine if not already done
cmake -B build -G Ninja
cmake --build build

# Configure Docker for GCR
gcloud auth configure-docker

# Build and push image
cd gcp_deploy
docker compose build
docker compose push
```

### 3. Get Tunnel Token

```bash
# List tunnels
cloudflared tunnel list

# Get token for 'sunshine' tunnel
cloudflared tunnel token sunshine
```

### 4. Create GCP VM

```bash
# Get tunnel token
TUNNEL_TOKEN=$(cloudflared tunnel token gcp-gaming)

# Create VM (edit create-vm.sh if needed)
export GCP_PROJECT=john-cli
export DOCKER_IMAGE=gcr.io/john-cli/sunshine-webrtc:latest
chmod +x create-vm.sh
./create-vm.sh sunshine-gpu us-west1-a john-cli
```

### 5. Deploy Cloudflare Worker

The worker is already deployed at `sunshine-cloud-gaming.makeshifted.workers.dev`.

To redeploy after changes:
```bash
cd cloudflare_worker
npm install --include=dev
wrangler deploy
```

### 6. Set Worker Secrets (already configured)

These secrets are already set, but for reference:

```bash
cd cloudflare_worker

# Create KV namespace (already done: 611d2f05129443cc81d3f9e4f95e91a7)
wrangler kv namespace create "SUNSHINE_STATE"

# Set secrets
echo "john-cli" | wrangler secret put GCP_PROJECT
echo "us-west1-a" | wrangler secret put GCP_ZONE
echo "sunshine-gpu" | wrangler secret put GCP_INSTANCE
echo "gaming.sels.tech" | wrangler secret put SUNSHINE_TUNNEL_URL

# Service account key (base64 encoded)
cat /tmp/sunshine-worker-key.json | base64 -w0 | wrangler secret put GCP_SERVICE_ACCOUNT_JSON
```

## GCP Service Account

A dedicated service account was created for the worker to manage VM instances:

- **Email**: `sunshine-worker@john-cli.iam.gserviceaccount.com`
- **Role**: `roles/compute.instanceAdmin.v1` (can start/stop/manage VMs)
- **Key**: Stored as base64-encoded secret `GCP_SERVICE_ACCOUNT_JSON` in worker

To recreate the service account:
```bash
# Create account
gcloud iam service-accounts create sunshine-worker \
  --display-name="Sunshine Worker VM Manager" \
  --project=john-cli

# Grant permissions
gcloud projects add-iam-policy-binding john-cli \
  --member="serviceAccount:sunshine-worker@john-cli.iam.gserviceaccount.com" \
  --role="roles/compute.instanceAdmin.v1"

# Create key
gcloud iam service-accounts keys create sunshine-worker-key.json \
  --iam-account=sunshine-worker@john-cli.iam.gserviceaccount.com

# Base64 encode and set as secret
cat sunshine-worker-key.json | base64 -w0 | wrangler secret put GCP_SERVICE_ACCOUNT_JSON
```

## Cloudflare Tunnel Configuration

The architecture uses two layers:

1. **Worker Custom Domains** (`gaming.sels.tech`, `play.sels.tech`): Route to Cloudflare Worker
2. **Internal Tunnel** (`sunshine-stream.sels.tech`): Route to VM's FastAPI proxy, embedded as iframe

**GCP Gaming Tunnel** (`gcp-gaming`):
- ID: `94ecba9a-99ef-4d73-88f5-491a98e85205`
- Internal Hostname: `sunshine-stream.sels.tech`
- Service: `http://localhost:8080` (FastAPI proxy in container)

**For GCP Docker container**, the tunnel runs in token mode:
- The container uses `cloudflared tunnel run` with `TUNNEL_TOKEN` environment variable
- The tunnel configuration is managed via Cloudflare API (not local config file)
- Get tunnel token: `cloudflared tunnel token gcp-gaming`

**Configure tunnel ingress via API:**
```bash
# Extract API token from cloudflared cert
API_TOKEN=$(cat ~/.cloudflared/cert.pem | grep -v "BEGIN\|END" | tr -d '\n' | base64 -d | jq -r '.apiToken')

# Configure tunnel ingress rules (internal hostname only)
curl "https://api.cloudflare.com/client/v4/accounts/ab77c04897c9dcc5f04b753c51dbe517/cfd_tunnel/94ecba9a-99ef-4d73-88f5-491a98e85205/configurations" \
  --request PUT \
  --header "Authorization: Bearer $API_TOKEN" \
  --header "Content-Type: application/json" \
  --data '{
    "config": {
      "ingress": [
        {"hostname": "sunshine-stream.sels.tech", "service": "http://localhost:8080"},
        {"service": "http_status:404"}
      ]
    }
  }'
```

## VM Management Commands

```bash
# Start VM
gcloud compute instances start sunshine-gpu --zone=us-west1-a

# Stop VM
gcloud compute instances stop sunshine-gpu --zone=us-west1-a

# SSH into VM
gcloud compute ssh sunshine-gpu --zone=us-west1-a

# View startup logs
gcloud compute ssh sunshine-gpu --zone=us-west1-a --command='sudo tail -f /var/log/startup.log'

# View Docker container logs
gcloud compute ssh sunshine-gpu --zone=us-west1-a --command='sudo docker logs sunshine'

# Check GPU status
gcloud compute ssh sunshine-gpu --zone=us-west1-a --command='nvidia-smi'

# Delete VM
gcloud compute instances delete sunshine-gpu --zone=us-west1-a
```

## Docker Container Services

The container runs these services via supervisord:

1. **Xorg** (priority 10): GPU-accelerated X display at :99
2. **Desktop** (priority 20): XFCE desktop environment
3. **Sunshine** (priority 30): WebRTC streaming server on ports 47989-47991
4. **Proxy** (priority 40): FastAPI reverse proxy on port 8080
5. **Cloudflared** (priority 50): Cloudflare tunnel connection

## Worker Endpoints

The Cloudflare Worker exposes these endpoints:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/` | GET | Streaming page - auto-starts VM, shows loading, embeds iframe when ready |
| `/api/vm-status` | GET | Returns VM status (RUNNING, TERMINATED, etc.) |
| `/api/start-vm` | POST | Starts the GCP VM |
| `/api/stop-vm` | POST | Stops the GCP VM |
| `/api/sunshine-status` | GET | Returns Sunshine readiness and connection status |
| `/api/heartbeat` | POST | Client heartbeat to track active viewers |
| `/api/idle-time` | GET | Returns seconds since last heartbeat (for debugging) |

## Auto-Shutdown

The worker uses client heartbeats to track activity:
1. Client page sends heartbeat every 30 seconds while open
2. Cron job runs every minute to check idle time
3. If no heartbeats for 5+ minutes, VM is shutdown

Timeout is configured in `cloudflare_worker/src/index.ts`:
```typescript
const IDLE_TIMEOUT_SECONDS = 5 * 60;  // 5 minutes
const HEARTBEAT_INTERVAL_SECONDS = 30; // Client heartbeat interval
```

## Cost Estimation

| Resource | Hourly Cost | Monthly (10hrs/day) |
|----------|-------------|---------------------|
| G2-standard-4 VM | ~$0.22 | ~$66 |
| L4 GPU | ~$0.53 | ~$159 |
| 100GB SSD | ~$0.02 | ~$17 |
| **Total** | **~$0.77** | **~$242** |

With auto-shutdown, costs are only incurred during active use.

## Troubleshooting

### Worker errors

```bash
# View worker logs
wrangler tail sunshine-cloud-gaming

# Check secrets
wrangler secret list
```

### VM won't start

```bash
# Check status
gcloud compute instances describe sunshine-gpu --zone=us-west1-a --format='get(status)'

# Check for quota issues
gcloud compute regions describe us-west1 | grep -A 2 "NVIDIA_L4"
```

### Container issues

```bash
# SSH and check Docker
gcloud compute ssh sunshine-gpu --zone=us-west1-a

# Inside VM:
sudo docker ps
sudo docker logs sunshine
sudo docker exec sunshine nvidia-smi
sudo docker exec sunshine cat /var/log/xorg.log
sudo docker exec sunshine cat /var/log/sunshine.log
```

### Tunnel not connecting

```bash
# Check tunnel status on VM
gcloud compute ssh sunshine-gpu --zone=us-west1-a --command='sudo docker exec sunshine cat /var/log/cloudflared.log'

# Verify tunnel exists
cloudflared tunnel list
cloudflared tunnel info gcp-gaming
```

## Security Notes

1. **Cloudflare Tunnel**: All traffic goes through Cloudflare, no exposed ports on VM
2. **Service Account**: Minimal permissions (only Compute Instance Admin)
3. **VM Firewall**: Default rules block all incoming except internal
4. **Auto-shutdown**: Prevents runaway costs and reduces attack surface
5. **Secrets**: All credentials stored as encrypted wrangler secrets

## License

Same as Sunshine project license.
