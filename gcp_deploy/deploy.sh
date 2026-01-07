#!/bin/bash
# Sunshine GCP Deployment Script
# Builds Docker image directly on GCP VM to avoid slow image transfers

set -e

ZONE="us-west1-a"
VM_NAME="sunshine-gpu"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUNSHINE_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=== Sunshine GCP Deployment ==="
echo "Copying build artifacts and configs to GCP VM..."

# Create remote build directory
gcloud compute ssh $VM_NAME --zone=$ZONE --command="mkdir -p /tmp/sunshine-build"

# Copy required files using gcloud compute scp
# - build/ directory (pre-built Sunshine binaries)
# - src_assets/ (web assets)
# - gcp_deploy/ (Dockerfile and configs)

echo "Syncing build directory..."
gcloud compute scp --recurse "$SUNSHINE_ROOT/build" $VM_NAME:/tmp/sunshine-build/ --zone=$ZONE

echo "Syncing web assets..."
gcloud compute scp --recurse "$SUNSHINE_ROOT/src_assets" $VM_NAME:/tmp/sunshine-build/ --zone=$ZONE

echo "Syncing gcp_deploy configs..."
gcloud compute scp --recurse "$SCRIPT_DIR" $VM_NAME:/tmp/sunshine-build/ --zone=$ZONE

echo "Building Docker image on GCP VM..."
gcloud compute ssh $VM_NAME --zone=$ZONE --command="cd /tmp/sunshine-build && sudo docker build -t sunshine-gcp:latest -f gcp_deploy/Dockerfile ."

echo "Restarting container with new image..."
gcloud compute ssh $VM_NAME --zone=$ZONE --command="
sudo docker stop sunshine 2>/dev/null || true
sudo docker rm sunshine 2>/dev/null || true
sudo mkdir -p /data/Downloads
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
  -v /usr/lib/x86_64-linux-gnu/libnvidia-glvkspirv.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvidia-glvkspirv.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvidia-gpucomp.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvidia-gpucomp.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvidia-glsi.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvidia-glsi.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvidia-eglcore.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvidia-eglcore.so.570.195.03:ro \
  -v /usr/lib/x86_64-linux-gnu/libnvidia-tls.so.570.195.03:/usr/lib/x86_64-linux-gnu/libnvidia-tls.so.570.195.03:ro \
  --restart unless-stopped \
  sunshine-gcp:latest
"

echo "Waiting for container to start..."
sleep 10

echo "Verifying deployment..."
gcloud compute ssh $VM_NAME --zone=$ZONE --command="curl -sk https://localhost/health"

echo ""
echo "=== Deployment Complete ==="
echo "Stream URL: https://stream.sels.tech/play"
