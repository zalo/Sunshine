# Sunshine GCP Deployment Stack

WebRTC game streaming server running on GCP with NVIDIA GPU acceleration.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                     GCP VM (us-west1-a)                         │
│                     NVIDIA L4 GPU + Driver 570                  │
├─────────────────────────────────────────────────────────────────┤
│  Docker Container (sunshine-gcp:latest)                         │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Supervisord (process manager)                            │  │
│  │  ├── Xorg :99 (NVIDIA driver, hardware-accelerated)       │  │
│  │  ├── XFCE4 Desktop + Chrome                               │  │
│  │  ├── Sunshine (WebRTC streaming server)                   │  │
│  │  └── FastAPI Proxy (ports 80/443)                         │  │
│  └───────────────────────────────────────────────────────────┘  │
├─────────────────────────────────────────────────────────────────┤
│  Volume Mounts:                                                 │
│  - /data/sunshine:/data (persistent config/state)              │
│  - /etc/letsencrypt (SSL certs via Let's Encrypt DNS-01)       │
│  - NVIDIA libs (encode, cuvid, cuda) for NVENC                 │
└─────────────────────────────────────────────────────────────────┘
         │
         │ Port 443: HTTPS/WSS (Let's Encrypt SSL)
         ▼
┌─────────────────────────────────────────────────────────────────┐
│  Cloudflare DNS (DNS-only, not proxied)                         │
│  - stream.sels.tech → VM static IP (34.53.41.58)               │
└─────────────────────────────────────────────────────────────────┘
         │
         ▼
    Browser Client (play.js)
    - WebRTC video/audio
    - DataChannel for input (keyboard/mouse/gamepad)
```

## Files

| File | Purpose |
|------|---------|
| `Dockerfile` | Container image with CUDA 12.8, Ubuntu 24.04, NVIDIA Xorg driver 570, XFCE, Chrome |
| `start.sh` | Startup script for each service (display, desktop, sunshine, proxy) |
| `supervisord.conf` | Process manager config - starts all services in order |
| `xorg.conf` | NVIDIA Xorg configuration for headless GPU rendering |
| `edid.bin` | Fake EDID for virtual monitor (required for headless NVIDIA) |
| `sunshine.conf` | Sunshine streaming server configuration |
| `proxy.py` | FastAPI reverse proxy consolidating HTTP + WebSocket on ports 80/443 |

## Key Technical Details

### NVIDIA Xorg (Hardware-Accelerated Desktop)

The container uses the real NVIDIA Xorg driver (not Xvfb) for GPU-accelerated desktop rendering:

- **Driver**: `xserver-xorg-video-nvidia-570` (must match host driver)
- **Headless trick**: Uses fake EDID (`edid.bin`) to convince driver a monitor is connected
- **xorg.conf options**:
  - `ConnectedMonitor "DFP-0"` - pretend DFP-0 is connected
  - `CustomEDID "DFP-0:/etc/X11/edid.bin"` - use our fake monitor definition
  - `AllowEmptyInitialConfiguration "True"` - allow starting without real display

### NVENC Hardware Encoding

Sunshine uses NVIDIA's hardware encoder for H.264/HEVC/AV1:

- **Required libraries** (must be mounted from host):
  - `libnvidia-encode.so.1` - NVENC encoder
  - `libnvcuvid.so.1` - CUDA video decoder
  - `libcuda.so.1` - CUDA runtime

The NVIDIA container toolkit doesn't auto-mount these, so they must be explicitly mounted.

### Desktop Startup Timing

The `start_desktop()` function waits for Xorg before starting XFCE:

```bash
for i in {1..30}; do
    if xdpyinfo -display :99 > /dev/null 2>&1; then
        break
    fi
    sleep 1
done
```

Chrome singleton locks are cleared before launch to prevent "profile in use" errors.

### WebSocket Routing

- `stream.sels.tech` is DNS-only (not Cloudflare proxied) pointing to VM static IP
- Let's Encrypt SSL via DNS-01 challenge (works without HTTP access)
- `play.js` connects to `wss://stream.sels.tech/ws/signaling` for WebRTC signaling
- All traffic goes directly to the VM on port 443

## Build & Deploy

### Build Image

```bash
cd /path/to/Sunshine
docker build -t sunshine-gcp:latest -f gcp_deploy/Dockerfile .
```

### Transfer to GCP

```bash
docker save sunshine-gcp:latest | gzip > /tmp/sunshine-gcp.tar.gz
gcloud compute scp /tmp/sunshine-gcp.tar.gz sunshine-gpu:/tmp/ --zone=us-west1-a
```

### Deploy Container

```bash
gcloud compute ssh sunshine-gpu --zone=us-west1-a

# Load image
gunzip -c /tmp/sunshine-gcp.tar.gz | sudo docker load

# Stop and remove old container (if exists)
sudo docker stop sunshine 2>/dev/null; sudo docker rm sunshine 2>/dev/null

# Create Downloads directory on host (for ROM storage, etc.)
sudo mkdir -p /data/Downloads

# Run container
sudo docker run -d --name sunshine \
  --gpus all \
  --privileged \
  -p 443:443 \
  -v /data/sunshine:/data \
  -v /data/Downloads:/data/Downloads \
  -v /etc/letsencrypt:/etc/letsencrypt:ro \
  -v /run/dbus:/run/dbus:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvidia-encode.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvidia-encode.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvidia-encode.so.1:/usr/lib/x86_64-linux-gnu/libnvidia-encode.so.1:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvcuvid.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvcuvid.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvcuvid.so.1:/usr/lib/x86_64-linux-gnu/libnvcuvid.so.1:ro \
  -v /usr/lib/x86_64-linux-gnu/libcuda.so.570.195.03:/usr/lib/x86_64-linux-gnu/libcuda.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libcuda.so.1:/usr/lib/x86_64-linux-gnu/libcuda.so.1:ro \
  --restart unless-stopped \
  sunshine-gcp:latest
```

## Host Setup (One-Time)

### Install NVIDIA Driver 570

```bash
sudo apt update
sudo apt install nvidia-driver-570
sudo reboot
```

### Setup Let's Encrypt SSL (DNS-01 Challenge)

Using DNS-01 challenge allows cert issuance/renewal without opening port 80:

```bash
# Install certbot with Cloudflare DNS plugin
sudo apt install certbot python3-certbot-dns-cloudflare

# Create Cloudflare API credentials
# Get API token from: https://dash.cloudflare.com/profile/api-tokens
# Token needs Zone:DNS:Edit permission for sels.tech
sudo mkdir -p /etc/letsencrypt
cat << 'EOF' | sudo tee /etc/letsencrypt/cloudflare.ini
dns_cloudflare_api_token = YOUR_CLOUDFLARE_API_TOKEN
EOF
sudo chmod 600 /etc/letsencrypt/cloudflare.ini

# Issue certificate (60s propagation needed for Cloudflare DNS)
sudo certbot certonly \
  --dns-cloudflare \
  --dns-cloudflare-credentials /etc/letsencrypt/cloudflare.ini \
  --dns-cloudflare-propagation-seconds 60 \
  -d stream.sels.tech

# Certbot auto-renewal is handled by systemd timer (certbot.timer)
```

### Firewall Rules

```bash
gcloud compute firewall-rules create allow-sunshine-ssl \
  --allow tcp:443 \
  --target-tags sunshine-server

gcloud compute instances add-tags sunshine-gpu \
  --tags sunshine-server \
  --zone us-west1-a
```

## Debugging

### Check Services

```bash
docker exec sunshine supervisorctl status
docker exec sunshine cat /var/log/xorg.log
docker exec sunshine cat /var/log/desktop.log
docker exec sunshine cat /var/log/sunshine.log
docker exec sunshine cat /var/log/proxy.log
```

### Verify NVENC

```bash
docker exec sunshine cat /var/log/sunshine.log | grep -i nvenc
# Should show: "Found H.264 encoder: h264_nvenc [nvenc]"
```

### Verify NVIDIA Xorg

```bash
docker exec sunshine cat /var/log/xorg.log | grep "GPU acceleration"
# Should show: "Xorg with NVIDIA GPU acceleration started successfully!"
```

### Check Desktop

```bash
docker exec sunshine bash -c 'DISPLAY=:99 xrandr'
docker exec sunshine bash -c 'ps aux | grep -E "(xfce|chrome)"'
```

## Common Issues

| Issue | Solution |
|-------|----------|
| "no screens found" in Xorg | Check BusID in xorg.conf matches `lspci \| grep NVIDIA` |
| "Cannot load libnvidia-encode.so.1" | Mount NVENC libs with correct version number |
| Chrome "profile in use" | Clear `/data/.config/google-chrome/Singleton*` |
| Desktop not starting | Check desktop waits for X display (xdpyinfo loop) |
| WebSocket 404 | Ensure stream.sels.tech is DNS-only in Cloudflare (not proxied) |
| Black video in browser | Hardware-accelerated video - click to unmute/play |

## VM Details

- **Name**: sunshine-gpu
- **Zone**: us-west1-a (Oregon)
- **GPU**: NVIDIA L4
- **Driver**: 570.195.03
- **Static IP**: 34.53.41.58
