#!/bin/bash
# Sunshine GCP Deployment Script
# Builds Docker image directly on GCP VM to avoid slow image transfers
# Uses GCS rsync for efficient incremental transfers

set -e

ZONE="us-west1-a"
VM_NAME="sunshine-gpu"
PROJECT="john-cli"
GCS_BUCKET="gs://${PROJECT}-sunshine-deploy"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SUNSHINE_ROOT="$(dirname "$SCRIPT_DIR")"

echo "=== Sunshine GCP Deployment ==="

# Ensure GCS bucket exists
echo "Ensuring GCS bucket exists..."
gcloud storage buckets describe "$GCS_BUCKET" &>/dev/null || \
    gcloud storage buckets create "$GCS_BUCKET" --location=us-west1

# Sync to GCS (incremental - only changed files)
echo "Syncing build directory to GCS (incremental)..."
gcloud storage rsync -r --delete-unmatched-destination-objects \
    --exclude="CMakeFiles/|CMakeCache.txt|.*\\.ninja|_deps/boost-src/" \
    "$SUNSHINE_ROOT/build" "$GCS_BUCKET/build"

echo "Syncing web assets to GCS..."
gcloud storage rsync -r --delete-unmatched-destination-objects \
    "$SUNSHINE_ROOT/src_assets" "$GCS_BUCKET/src_assets"

echo "Syncing gcp_deploy configs to GCS..."
gcloud storage rsync -r --delete-unmatched-destination-objects \
    --exclude="cloudflare_worker/node_modules/" \
    "$SCRIPT_DIR" "$GCS_BUCKET/gcp_deploy"

# Create remote build directory and sync from GCS
echo "Syncing from GCS to VM (incremental)..."
gcloud compute ssh $VM_NAME --zone=$ZONE --command="
    mkdir -p /tmp/sunshine-build/build /tmp/sunshine-build/src_assets /tmp/sunshine-build/gcp_deploy
    gsutil -m rsync -r -d $GCS_BUCKET/build /tmp/sunshine-build/build
    gsutil -m rsync -r -d $GCS_BUCKET/src_assets /tmp/sunshine-build/src_assets
    gsutil -m rsync -r -d $GCS_BUCKET/gcp_deploy /tmp/sunshine-build/gcp_deploy

    # GCS rsync doesn't preserve symlinks or execute permissions - fix them
    chmod +x /tmp/sunshine-build/build/sunshine-0.0.0.dirty
    ln -sf sunshine-0.0.0.dirty /tmp/sunshine-build/build/sunshine

    # Make lib_deps libraries executable
    chmod +x /tmp/sunshine-build/build/lib_deps/*.so* 2>/dev/null || true
"

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
  --shm-size=15g \
  -p 443:443 \
  -p 8080:8080 \
  -p 40000-40100:40000-40100/udp \
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
