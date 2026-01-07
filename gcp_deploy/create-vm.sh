#!/bin/bash
# Create or update GCP L4 GPU VM for Sunshine WebRTC streaming
#
# This script is idempotent - it can be run multiple times:
# - If VM doesn't exist: creates it
# - If VM exists: updates metadata and restarts to pull new image
#
# Prerequisites:
# 1. gcloud CLI authenticated
# 2. GPU quota for L4 in your desired zone
# 3. Docker image pushed to GCR
#
# Usage:
#   ./create-vm.sh [INSTANCE_NAME] [ZONE] [PROJECT_ID]

set -e

# Configuration
INSTANCE_NAME="${1:-sunshine-gpu}"
ZONE="${2:-us-west1-a}"
PROJECT="${3:-$(gcloud config get-value project 2>/dev/null)}"
MACHINE_TYPE="g2-standard-4"
DISK_SIZE="100"
DISK_TYPE="pd-ssd"

# Docker image
DOCKER_IMAGE="${DOCKER_IMAGE:-gcr.io/${PROJECT}/sunshine-webrtc:latest}"

# Cloudflare tunnel token
TUNNEL_TOKEN="${TUNNEL_TOKEN:-}"

echo "=============================================="
echo "GCP L4 GPU VM for Sunshine"
echo "=============================================="
echo "Instance: ${INSTANCE_NAME}"
echo "Zone: ${ZONE}"
echo "Project: ${PROJECT}"
echo "Machine Type: ${MACHINE_TYPE}"
echo "Docker Image: ${DOCKER_IMAGE}"
echo ""

if [ -z "$TUNNEL_TOKEN" ]; then
    echo "ERROR: TUNNEL_TOKEN not set."
    echo "Get token with: cloudflared tunnel token gcp-gaming"
    echo "Then run: TUNNEL_TOKEN=<token> ./create-vm.sh"
    exit 1
fi

# Write startup script to temp file
# This script runs on EVERY boot, not just first boot
STARTUP_SCRIPT_FILE=$(mktemp)
cat > "$STARTUP_SCRIPT_FILE" << 'STARTUP_EOF'
#!/bin/bash
set -e

log() {
    echo "[$(date -Iseconds)] $1" | tee -a /var/log/startup.log
}

LOCKFILE=/var/run/startup-script.lock
exec 200>$LOCKFILE
flock -n 200 || { log "Another startup script is running"; exit 0; }

log "=========================================="
log "Starting VM initialization..."
log "=========================================="

# Check if Docker is installed
if ! command -v docker &> /dev/null; then
    log "Installing Docker..."
    apt-get update
    apt-get install -y apt-transport-https ca-certificates curl gnupg lsb-release

    curl -fsSL https://download.docker.com/linux/ubuntu/gpg | gpg --dearmor -o /usr/share/keyrings/docker-archive-keyring.gpg
    echo "deb [arch=amd64 signed-by=/usr/share/keyrings/docker-archive-keyring.gpg] https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" > /etc/apt/sources.list.d/docker.list

    apt-get update
    apt-get install -y docker-ce docker-ce-cli containerd.io
fi

# Check if NVIDIA Container Toolkit is installed
if ! command -v nvidia-ctk &> /dev/null; then
    log "Installing NVIDIA Container Toolkit..."
    curl -fsSL https://nvidia.github.io/libnvidia-container/gpgkey | gpg --dearmor -o /usr/share/keyrings/nvidia-container-toolkit-keyring.gpg
    curl -s -L https://nvidia.github.io/libnvidia-container/stable/deb/nvidia-container-toolkit.list | \
        sed 's#deb https://#deb [signed-by=/usr/share/keyrings/nvidia-container-toolkit-keyring.gpg] https://#g' > /etc/apt/sources.list.d/nvidia-container-toolkit.list

    apt-get update
    apt-get install -y nvidia-container-toolkit

    # Configure Docker to use NVIDIA runtime
    nvidia-ctk runtime configure --runtime=docker
    systemctl restart docker
fi

# Check NVIDIA drivers
log "Checking NVIDIA drivers..."
nvidia-smi || {
    log "NVIDIA drivers not working - installing..."
    apt-get install -y nvidia-driver-535
    log "Drivers installed - rebooting..."
    reboot
    exit 0
}

# Get configuration from metadata
DOCKER_IMAGE=$(curl -s "http://metadata.google.internal/computeMetadata/v1/instance/attributes/docker-image" -H "Metadata-Flavor: Google")
TUNNEL_TOKEN=$(curl -s "http://metadata.google.internal/computeMetadata/v1/instance/attributes/tunnel-token" -H "Metadata-Flavor: Google")

if [ -z "$DOCKER_IMAGE" ]; then
    log "ERROR: docker-image metadata not set"
    exit 1
fi

# Save tunnel token for container restarts
echo "$TUNNEL_TOKEN" > /tmp/tunnel_token

log "Authenticating to GCR..."
gcloud auth configure-docker gcr.io --quiet

# Always pull latest image
log "Pulling Docker image: $DOCKER_IMAGE"
docker pull "$DOCKER_IMAGE"

# Check if container exists and is using the right image
CURRENT_IMAGE=$(docker inspect sunshine --format='{{.Image}}' 2>/dev/null || echo "none")
PULLED_IMAGE=$(docker inspect "$DOCKER_IMAGE" --format='{{.Id}}' 2>/dev/null || echo "unknown")

if [ "$CURRENT_IMAGE" != "$PULLED_IMAGE" ]; then
    log "Image changed - recreating container..."
    docker stop sunshine 2>/dev/null || true
    docker rm sunshine 2>/dev/null || true

    log "Starting Sunshine container with new image..."
    docker run -d \
        --name sunshine \
        --restart unless-stopped \
        --gpus all \
        --privileged \
        -e TUNNEL_TOKEN="$TUNNEL_TOKEN" \
        -p 8080:8080 \
        -v sunshine-data:/data \
        "$DOCKER_IMAGE"
else
    # Just make sure container is running
    if ! docker ps | grep -q sunshine; then
        log "Container not running - starting..."
        docker start sunshine 2>/dev/null || {
            log "Failed to start, recreating..."
            docker rm sunshine 2>/dev/null || true
            docker run -d \
                --name sunshine \
                --restart unless-stopped \
                --gpus all \
                --privileged \
                -e TUNNEL_TOKEN="$TUNNEL_TOKEN" \
                -p 8080:8080 \
                -v sunshine-data:/data \
                "$DOCKER_IMAGE"
        }
    else
        log "Container already running with correct image"
    fi
fi

log "Container status:"
docker ps
log "=========================================="
log "Startup complete!"
log "=========================================="
STARTUP_EOF

# Check if VM already exists
if gcloud compute instances describe "$INSTANCE_NAME" --zone="$ZONE" --project="$PROJECT" &>/dev/null; then
    echo "VM already exists - updating metadata and restarting..."

    # Update metadata
    gcloud compute instances add-metadata "$INSTANCE_NAME" \
        --project="$PROJECT" \
        --zone="$ZONE" \
        --metadata="docker-image=${DOCKER_IMAGE},tunnel-token=${TUNNEL_TOKEN}"

    # Update startup script
    gcloud compute instances add-metadata "$INSTANCE_NAME" \
        --project="$PROJECT" \
        --zone="$ZONE" \
        --metadata-from-file="startup-script=${STARTUP_SCRIPT_FILE}"

    # Get current status
    STATUS=$(gcloud compute instances describe "$INSTANCE_NAME" --zone="$ZONE" --project="$PROJECT" --format='get(status)')

    if [ "$STATUS" = "RUNNING" ]; then
        echo "VM is running - restarting to apply updates..."
        gcloud compute instances reset "$INSTANCE_NAME" --zone="$ZONE" --project="$PROJECT"
    else
        echo "VM is stopped - starting..."
        gcloud compute instances start "$INSTANCE_NAME" --zone="$ZONE" --project="$PROJECT"
    fi
else
    echo "Creating new VM instance..."
    gcloud compute instances create "$INSTANCE_NAME" \
        --project="$PROJECT" \
        --zone="$ZONE" \
        --machine-type="$MACHINE_TYPE" \
        --accelerator="type=nvidia-l4,count=1" \
        --maintenance-policy="TERMINATE" \
        --provisioning-model="STANDARD" \
        --image-family="ubuntu-2204-lts" \
        --image-project="ubuntu-os-cloud" \
        --boot-disk-size="${DISK_SIZE}GB" \
        --boot-disk-type="$DISK_TYPE" \
        --boot-disk-device-name="$INSTANCE_NAME" \
        --metadata="docker-image=${DOCKER_IMAGE},tunnel-token=${TUNNEL_TOKEN}" \
        --metadata-from-file="startup-script=${STARTUP_SCRIPT_FILE}" \
        --scopes="https://www.googleapis.com/auth/cloud-platform" \
        --tags="sunshine,http-server,https-server"
fi

# Cleanup
rm -f "$STARTUP_SCRIPT_FILE"

echo ""
echo "=============================================="
echo "VM Operation Complete!"
echo "=============================================="
echo ""
echo "Instance: ${INSTANCE_NAME}"
echo "Zone: ${ZONE}"
echo ""
echo "Commands:"
echo "  gcloud compute ssh ${INSTANCE_NAME} --zone=${ZONE}"
echo "  gcloud compute ssh ${INSTANCE_NAME} --zone=${ZONE} --command='sudo tail -f /var/log/startup.log'"
echo "  gcloud compute instances stop ${INSTANCE_NAME} --zone=${ZONE}"
echo "  gcloud compute instances start ${INSTANCE_NAME} --zone=${ZONE}"
echo ""
echo "To deploy a new Docker image:"
echo "  1. docker compose build && docker compose push"
echo "  2. TUNNEL_TOKEN=\$(cloudflared tunnel token gcp-gaming) ./create-vm.sh"
echo ""
