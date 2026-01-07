# Sunshine WebRTC Streaming Server - GCP Deployment
# Based on Modal deployment, adapted for bare-metal GPU access
#
# This provides full GPU-accelerated desktop rendering via NVIDIA Xorg driver
# (not possible on Modal due to gVisor limitations)

# CUDA 13.1 with Ubuntu 24.04 for latest features
FROM nvidia/cuda:13.1.0-runtime-ubuntu24.04

# Environment
ENV DEBIAN_FRONTEND=noninteractive
ENV DISPLAY=:99
ENV HOME=/data
ENV XDG_CONFIG_HOME=/data/.config
ENV XDG_DATA_HOME=/data/.local/share
ENV XDG_CACHE_HOME=/data/.cache

# =============================================================================
# Runtime dependencies and desktop environment
# =============================================================================
RUN apt-get update && apt-get install -y --no-install-recommends \
    # Python for proxy
    python3 python3-pip python-is-python3 \
    # Runtime dependencies for Sunshine
    libssl3 libcurl4 libminiupnpc17 \
    libopus0 libpulse0 \
    # X11 and display (use Xvfb for virtual framebuffer, NVENC for encoding)
    xserver-xorg-core xserver-xorg-video-dummy \
    x11-utils libx11-6 libxcb1 \
    libxfixes3 libxrandr2 libxtst6 \
    libxcb-shm0 libxcb-xfixes0 \
    xvfb \
    # Video encoding
    libva2 libva-drm2 libva-x11-2 \
    libdrm2 libgbm1 \
    # Audio
    libasound2t64 pulseaudio \
    # Other runtime libs
    libcap2 libnuma1 \
    libsystemd0 libudev1 \
    libicu74 \
    libevdev2 \
    # Desktop environment (XFCE - lightweight)
    xfce4 xfce4-terminal dbus-x11 \
    # Fonts for proper text rendering
    fonts-dejavu fonts-liberation fonts-noto-color-emoji \
    # Window manager utilities
    xdotool wmctrl \
    # For Chrome
    wget gnupg ca-certificates \
    # Utilities
    curl supervisor \
    && rm -rf /var/lib/apt/lists/*

# Install Google Chrome
RUN wget -q -O - https://dl.google.com/linux/linux_signing_key.pub | gpg --dearmor -o /usr/share/keyrings/google-chrome.gpg \
    && echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/google-chrome.gpg] http://dl.google.com/linux/chrome/deb/ stable main' > /etc/apt/sources.list.d/google-chrome.list \
    && apt-get update && apt-get install -y google-chrome-stable \
    && rm -rf /var/lib/apt/lists/*

# Install Python proxy dependencies
RUN pip install --break-system-packages \
    fastapi>=0.104.0 \
    'uvicorn[standard]>=0.24.0' \
    httpx>=0.25.0 \
    websockets>=12.0

# =============================================================================
# Copy Sunshine binary and assets
# =============================================================================
# Copy pre-built Sunshine (must be built locally first)
# Cache bust: 2026-01-06-v3-reconnect-fix
COPY build /opt/sunshine-build

# Copy updated web assets to OVERWRITE the ones in build directory
# This ensures the latest play.js with correct WebSocket routing is used
# Cache bust: 2026-01-06-v3-reconnect-fix
COPY src_assets/common/assets/web/public /opt/sunshine-build/assets/web/

# =============================================================================
# Configuration files
# =============================================================================
# Xorg config for NVIDIA headless rendering
COPY gcp_deploy/xorg.conf /etc/X11/xorg.conf

# Sunshine configuration
COPY gcp_deploy/sunshine.conf /opt/sunshine.conf

# Proxy server
COPY gcp_deploy/proxy.py /opt/proxy.py

# Startup script
COPY gcp_deploy/start.sh /opt/start.sh
RUN chmod +x /opt/start.sh

# Supervisor configuration
COPY gcp_deploy/supervisord.conf /etc/supervisor/conf.d/supervisord.conf

# Create data directory
RUN mkdir -p /data/sunshine /data/.config /data/.local/share /data/.cache /data/Desktop

# Create Chrome desktop shortcut
RUN echo '[Desktop Entry]\n\
Version=1.0\n\
Type=Application\n\
Name=Google Chrome\n\
Comment=Access the Internet\n\
Exec=/usr/bin/google-chrome-stable --no-sandbox --disable-gpu-sandbox %U\n\
Icon=google-chrome\n\
Terminal=false\n\
Categories=Network;WebBrowser;' > /data/Desktop/google-chrome.desktop \
    && chmod 755 /data/Desktop/google-chrome.desktop

# Expose proxy ports
# Port 80: HTTP for Cloudflare Flexible SSL (stream.sels.tech)
# Port 443: HTTPS for direct WebSocket access (ws-stream.sels.tech)
EXPOSE 80 443

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD curl -f http://localhost:80/health || exit 1

# Start all services via supervisor
CMD ["/usr/bin/supervisord", "-c", "/etc/supervisor/conf.d/supervisord.conf"]
