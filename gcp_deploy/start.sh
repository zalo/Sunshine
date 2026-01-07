#!/bin/bash
# Startup script for Sunshine WebRTC streaming on GCP
# This is called by supervisord to start individual services

set -e

# Function to log with timestamp
log() {
    echo "[$(date -Iseconds)] $1"
}

# =====================================================
# Start X Display (NVIDIA Xorg or Xvfb fallback)
# =====================================================
start_display() {
    log "Starting X display..."

    # Try NVIDIA Xorg first (GCP L4 has full GPU access)
    log "Attempting to start Xorg with NVIDIA driver..."

    # Check if NVIDIA driver is available
    if nvidia-smi > /dev/null 2>&1; then
        log "NVIDIA driver detected"
        nvidia-smi

        # Start Xorg
        Xorg :99 -config /etc/X11/xorg.conf -noreset +extension GLX &
        XORG_PID=$!
        sleep 3

        # Check if Xorg started successfully
        if kill -0 $XORG_PID 2>/dev/null; then
            export DISPLAY=:99
            if xdpyinfo > /dev/null 2>&1; then
                log "Xorg with NVIDIA GPU acceleration started successfully!"
                return 0
            fi
        fi

        # Xorg failed, kill it
        kill $XORG_PID 2>/dev/null || true
        log "Xorg failed, falling back to Xvfb"
    else
        log "NVIDIA driver not available, using Xvfb"
    fi

    # Fallback to Xvfb (software rendering)
    log "Starting Xvfb (software rendering)..."
    Xvfb :99 -screen 0 1920x1080x24 &
    sleep 2
    export DISPLAY=:99
    log "Xvfb started on display :99"
}

# =====================================================
# Start Desktop Environment
# =====================================================
start_desktop() {
    log "Starting XFCE desktop environment..."

    export DISPLAY=:99
    export HOME=/data
    export XDG_CONFIG_HOME=/data/.config
    export XDG_DATA_HOME=/data/.local/share
    export XDG_CACHE_HOME=/data/.cache
    export DBUS_SESSION_BUS_ADDRESS=""

    # Start dbus
    eval $(dbus-launch --sh-syntax)

    # Start XFCE
    startxfce4 &
    sleep 3

    # Open terminal
    xfce4-terminal --geometry=100x30+400+200 &
    sleep 1

    log "XFCE desktop started"
}

# =====================================================
# Start Sunshine
# =====================================================
start_sunshine() {
    log "Starting Sunshine..."

    BUILD_DIR="/opt/sunshine-build"
    CONFIG_PATH="/opt/sunshine.conf"

    # Set up library paths
    export LD_LIBRARY_PATH="${BUILD_DIR}/lib_deps:/usr/lib/x86_64-linux-gnu:/usr/local/cuda/lib64:/usr/local/nvidia/lib64:${LD_LIBRARY_PATH:-}"
    export DISPLAY=:99
    export HOME=/data
    export XDG_CONFIG_HOME=/data

    log "LD_LIBRARY_PATH: $LD_LIBRARY_PATH"

    # Wait for X display to be ready
    log "Waiting for X display :99..."
    for i in {1..30}; do
        if xdpyinfo -display :99 > /dev/null 2>&1; then
            log "X display :99 is ready!"
            break
        fi
        log "Waiting for X display... ($i/30)"
        sleep 1
    done

    if ! xdpyinfo -display :99 > /dev/null 2>&1; then
        log "ERROR: X display :99 not available after 30 seconds"
        exit 1
    fi

    # Run Sunshine
    cd "$BUILD_DIR"
    exec ./sunshine "$CONFIG_PATH"
}

# =====================================================
# Start Proxy
# =====================================================
start_proxy() {
    log "Starting FastAPI proxy..."
    cd /opt
    exec python proxy.py
}

# =====================================================
# Main - handle different service modes
# =====================================================
case "${1:-all}" in
    display)
        start_display
        # Keep running
        while true; do sleep 86400; done
        ;;
    desktop)
        start_desktop
        # Keep running
        while true; do sleep 86400; done
        ;;
    sunshine)
        start_sunshine
        ;;
    proxy)
        start_proxy
        ;;
    all)
        # Start everything in sequence
        start_display
        start_desktop
        start_sunshine &
        start_proxy
        ;;
    *)
        echo "Usage: $0 {display|desktop|sunshine|proxy|all}"
        exit 1
        ;;
esac
