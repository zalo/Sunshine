"""
Modal Deployment for Sunshine WebRTC Streaming Server

This deploys a pre-built Sunshine binary on Modal with:
- GPU support for hardware video encoding (NVENC)
- Virtual display (Xvfb) for headless operation
- Single-port HTTP/WebSocket via FastAPI proxy
- Auto-scaling based on demand

Usage:
    modal deploy modal_deploy/app.py  # Production deployment
    modal serve modal_deploy/app.py   # Development with hot reload
"""

import modal
import subprocess
import time
import os
import asyncio

# Define the Modal app
app = modal.App("sunshine-webrtc")

# =============================================================================
# LAYER 1: Ubuntu 24.04 base with runtime dependencies and desktop environment
# Modal provides NVIDIA drivers on GPU instances, Sunshine will auto-detect NVENC
# =============================================================================
base_image = (
    modal.Image.from_registry("nvidia/cuda:13.1.0-runtime-ubuntu24.04")
    .apt_install(
        # Python for Modal runtime and proxy
        "python3", "python3-pip", "python-is-python3",
        # Runtime dependencies for Sunshine
        "libssl3", "libcurl4", "libminiupnpc17",
        "libopus0", "libpulse0",
        # X11 and display
        "xvfb", "x11-utils", "libx11-6", "libxcb1",
        "libxfixes3", "libxrandr2", "libxtst6",
        "libxcb-shm0", "libxcb-xfixes0",
        # Video encoding
        "libva2", "libva-drm2", "libva-x11-2",
        "libdrm2", "libgbm1",
        # Audio
        "libasound2t64", "pulseaudio",
        # Other runtime libs
        "libcap2", "libnuma1",
        "libsystemd0", "libudev1",
        "libicu74",  # ICU for Boost.Locale
        "libevdev2",  # Input device library
        # Desktop environment (XFCE - lightweight)
        "xfce4", "xfce4-terminal", "dbus-x11",
        # Fonts for proper text rendering
        "fonts-dejavu", "fonts-liberation", "fonts-noto-color-emoji",
        # Window manager utilities
        "xdotool", "wmctrl",
        # For Chrome
        "wget", "gnupg", "ca-certificates",
    )
    # Install Google Chrome
    .run_commands(
        "wget -q -O - https://dl.google.com/linux/linux_signing_key.pub | gpg --dearmor -o /usr/share/keyrings/google-chrome.gpg",
        "echo 'deb [arch=amd64 signed-by=/usr/share/keyrings/google-chrome.gpg] http://dl.google.com/linux/chrome/deb/ stable main' > /etc/apt/sources.list.d/google-chrome.list",
        "apt-get update && apt-get install -y google-chrome-stable",
    )
    .env({
        "DISPLAY": ":99",
    })
)

# =============================================================================
# LAYER 2: Python proxy dependencies
# =============================================================================
python_image = base_image.run_commands(
    "pip install --break-system-packages fastapi>=0.104.0 'uvicorn[standard]>=0.24.0' httpx>=0.25.0 websockets>=12.0"
)

# =============================================================================
# LAYER 3: Pre-built Sunshine binary and libraries
# =============================================================================
# Copy the entire build directory (contains binary + shared libs)
build_image = python_image.add_local_dir(
    "/home/selstad/Desktop/Sunshine/build",
    remote_path="/opt/sunshine-build",
    copy=True,
    ignore=[
        "CMakeFiles/",
        "CMakeCache.txt",
        "cmake_install.cmake",
        "*.ninja",
        ".ninja*",
        "_deps/boost-src/",  # Source not needed, only built libs
        "libevdev-*/",  # Built from source, not needed at runtime
    ]
)

# =============================================================================
# LAYER 4: Web assets (changes frequently with UI updates)
# =============================================================================
final_image = build_image.add_local_dir(
    "/home/selstad/Desktop/Sunshine/src_assets/common/assets/web/public",
    remote_path="/opt/sunshine-web",
    copy=True,
)

# Volume for persistent state
sunshine_volume = modal.Volume.from_name("sunshine-data", create_if_missing=True)


# =============================================================================
# Runtime Functions
# =============================================================================

def start_xvfb():
    """Start Xvfb virtual display."""
    subprocess.Popen(
        ["Xvfb", ":99", "-screen", "0", "1920x1080x24"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    time.sleep(2)
    print("Xvfb started on display :99")


def start_desktop():
    """Start XFCE desktop environment with terminal and Chrome shortcut."""
    env = os.environ.copy()
    env["DISPLAY"] = ":99"
    env["HOME"] = "/data"
    env["XDG_CONFIG_HOME"] = "/data/.config"
    env["XDG_DATA_HOME"] = "/data/.local/share"
    env["XDG_CACHE_HOME"] = "/data/.cache"
    env["DBUS_SESSION_BUS_ADDRESS"] = ""

    # Create necessary directories
    os.makedirs("/data/.config", exist_ok=True)
    os.makedirs("/data/.local/share", exist_ok=True)
    os.makedirs("/data/.cache", exist_ok=True)
    os.makedirs("/data/Desktop", exist_ok=True)

    # Create Chrome desktop shortcut
    chrome_desktop = """[Desktop Entry]
Version=1.0
Type=Application
Name=Google Chrome
Comment=Access the Internet
Exec=/usr/bin/google-chrome-stable --no-sandbox --disable-gpu-sandbox %U
Icon=google-chrome
Terminal=false
Categories=Network;WebBrowser;
"""
    with open("/data/Desktop/google-chrome.desktop", "w") as f:
        f.write(chrome_desktop)
    os.chmod("/data/Desktop/google-chrome.desktop", 0o755)

    # Start dbus session
    dbus_proc = subprocess.Popen(
        ["dbus-launch", "--sh-syntax"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL
    )
    dbus_output = dbus_proc.communicate()[0].decode()
    for line in dbus_output.split('\n'):
        if '=' in line:
            key, value = line.split('=', 1)
            value = value.strip(';').strip("'").strip('"')
            env[key] = value

    # Start XFCE session
    subprocess.Popen(
        ["startxfce4"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    time.sleep(3)
    print("XFCE desktop started")

    # Open terminal in the center of the screen
    subprocess.Popen(
        ["xfce4-terminal", "--geometry=100x30+400+200"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    time.sleep(1)
    print("Terminal opened")


def start_sunshine():
    """Start the Sunshine server."""
    build_dir = "/opt/sunshine-build"

    # Debug: Check GPU availability
    print("=== Checking GPU availability ===")
    gpu_check = subprocess.run(["nvidia-smi"], capture_output=True, text=True)
    if gpu_check.returncode == 0:
        print("nvidia-smi output:")
        print(gpu_check.stdout[:2000])  # Print first 2000 chars
    else:
        print(f"nvidia-smi failed: {gpu_check.stderr}")

    # Debug: Check for NVENC libraries
    import glob
    nvenc_libs = glob.glob("/usr/lib/x86_64-linux-gnu/libnvidia-encode*")
    cuda_libs = glob.glob("/usr/lib/x86_64-linux-gnu/libcuda*")
    print(f"NVENC libraries found: {nvenc_libs}")
    print(f"CUDA libraries found: {cuda_libs}")

    # Set up library paths for the pre-built binary
    # All needed libs are in lib_deps (Boost is statically linked)
    lib_paths = [f"{build_dir}/lib_deps"]

    # Create data directory
    os.makedirs("/data/sunshine", exist_ok=True)

    # Create default config if not exists
    config_path = "/data/sunshine/sunshine.conf"
    if not os.path.exists(config_path):
        with open(config_path, "w") as f:
            f.write("""# Sunshine Configuration
sunshine_name = Modal Sunshine
port = 47989
address_family = ipv4

# WebRTC settings
webrtc_enabled = true
webrtc_stun_server = stun:stun.l.google.com:19302

# Encoder settings - try NVENC first, fall back to software
# Comment out encoder line to let Sunshine auto-detect

# Logging
min_log_level = info

# Web assets path
file_apps = /opt/sunshine-web
""")

    # Start Sunshine with library paths
    env = os.environ.copy()
    env["HOME"] = "/data"
    env["XDG_CONFIG_HOME"] = "/data"
    env["DISPLAY"] = ":99"
    # Add NVIDIA library paths for CUDA/NVENC
    nvidia_lib_paths = [
        "/usr/lib/x86_64-linux-gnu",
        "/usr/local/cuda/lib64",
        "/usr/local/nvidia/lib64",
    ]
    env["LD_LIBRARY_PATH"] = ":".join(lib_paths + nvidia_lib_paths) + ":" + env.get("LD_LIBRARY_PATH", "")

    # Run from build directory so relative paths (./assets) work
    sunshine_bin = "./sunshine"
    print(f"Starting Sunshine from {build_dir}")
    print(f"LD_LIBRARY_PATH: {env.get('LD_LIBRARY_PATH', '')}")

    proc = subprocess.Popen(
        [sunshine_bin, config_path],  # Config path is positional argument
        env=env,
        cwd=build_dir,  # Run from build directory for relative paths
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT
    )

    # Wait for startup and check logs (capture encoder probing output)
    time.sleep(3)

    # Read initial output to see encoder probing
    import select
    if proc.stdout:
        # Read available output without blocking
        import io
        output_lines = []
        while True:
            # Check if there's data available
            readable, _, _ = select.select([proc.stdout], [], [], 0.1)
            if not readable:
                break
            line = proc.stdout.readline()
            if not line:
                break
            output_lines.append(line.decode())
            if len(output_lines) > 100:  # Limit to first 100 lines
                break

        if output_lines:
            print("=== Sunshine startup output (encoder probing) ===")
            for line in output_lines:
                print(line, end='')
            print("=== End of startup output ===")

    time.sleep(2)

    if proc.poll() is not None:
        output = proc.stdout.read().decode() if proc.stdout else "No output"
        raise RuntimeError(f"Sunshine failed to start: {output}")

    print("Sunshine started successfully!")
    return proc


def create_proxy_app():
    """Create the FastAPI proxy application."""
    import ssl
    import httpx
    import websockets
    from fastapi import FastAPI, WebSocket, Request, Response
    from starlette.websockets import WebSocketDisconnect
    import logging

    logging.basicConfig(level=logging.INFO)
    logger = logging.getLogger(__name__)

    # Internal Sunshine server configuration
    SUNSHINE_HTTP_HOST = "127.0.0.1"
    SUNSHINE_HTTP_PORT = 47990
    SUNSHINE_WS_PORT = 47991

    # SSL context for self-signed cert
    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    web_app = FastAPI(title="Sunshine WebRTC Proxy")

    @web_app.websocket("/ws/signaling")
    async def websocket_proxy(websocket: WebSocket):
        """Proxy WebSocket connections to internal signaling server."""
        await websocket.accept()
        logger.info("Client WebSocket connected")

        ws_url = f"wss://{SUNSHINE_HTTP_HOST}:{SUNSHINE_WS_PORT}"

        try:
            async with websockets.connect(
                ws_url,
                ssl=ssl_context,
                ping_interval=20,
                ping_timeout=20
            ) as sunshine_ws:
                logger.info("Connected to Sunshine WebSocket")

                async def client_to_sunshine():
                    try:
                        while True:
                            data = await websocket.receive_text()
                            await sunshine_ws.send(data)
                    except WebSocketDisconnect:
                        logger.info("Client disconnected")
                    except Exception as e:
                        logger.error(f"Client->Sunshine error: {e}")

                async def sunshine_to_client():
                    try:
                        async for message in sunshine_ws:
                            if isinstance(message, str):
                                await websocket.send_text(message)
                            else:
                                await websocket.send_bytes(message)
                    except Exception as e:
                        logger.error(f"Sunshine->Client error: {e}")

                await asyncio.gather(
                    client_to_sunshine(),
                    sunshine_to_client(),
                    return_exceptions=True
                )

        except Exception as e:
            logger.error(f"WebSocket proxy error: {e}")
            try:
                await websocket.close(code=1011, reason=str(e))
            except:
                pass

    @web_app.get("/health")
    async def health_check():
        """Health check endpoint."""
        return {"status": "healthy", "service": "sunshine-proxy"}

    @web_app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"])
    async def http_proxy(request: Request, path: str):
        """Proxy HTTP requests to internal HTTPS server."""
        url = f"https://{SUNSHINE_HTTP_HOST}:{SUNSHINE_HTTP_PORT}/{path}"
        if request.query_params:
            url += f"?{request.query_params}"

        body = await request.body()

        headers = dict(request.headers)
        hop_by_hop = ['connection', 'keep-alive', 'proxy-authenticate',
                      'proxy-authorization', 'te', 'trailers', 'transfer-encoding',
                      'upgrade', 'host']
        headers = {k: v for k, v in headers.items() if k.lower() not in hop_by_hop}

        async with httpx.AsyncClient(verify=False, timeout=30.0) as client:
            try:
                response = await client.request(
                    method=request.method,
                    url=url,
                    headers=headers,
                    content=body,
                    follow_redirects=False
                )

                response_headers = dict(response.headers)
                for header in hop_by_hop + ['content-encoding', 'content-length']:
                    response_headers.pop(header, None)

                return Response(
                    content=response.content,
                    status_code=response.status_code,
                    headers=response_headers,
                    media_type=response.headers.get('content-type')
                )

            except Exception as e:
                logger.error(f"HTTP proxy error: {e}")
                return Response(
                    content=f"Proxy error: {e}",
                    status_code=502,
                    media_type="text/plain"
                )

    return web_app


# Global process handles
_xvfb_started = False
_desktop_started = False
_sunshine_proc = None


@app.function(
    image=final_image,
    gpu="L4",  # Good cost/performance for video encoding
    volumes={"/data": sunshine_volume},
    timeout=86400,  # 24 hours max session
    scaledown_window=300,  # 5 min idle before shutdown
)
@modal.concurrent(max_inputs=100)  # Handle many WebSocket connections
@modal.asgi_app()
def sunshine_server():
    """
    Main Sunshine server entry point.

    Starts:
    1. Xvfb virtual display
    2. XFCE desktop environment with terminal
    3. Sunshine streaming server (internal ports 47990, 47991)
    4. FastAPI proxy (exposed via ASGI)
    """
    global _xvfb_started, _desktop_started, _sunshine_proc

    # Start services on first request
    if not _xvfb_started:
        start_xvfb()
        _xvfb_started = True

    if not _desktop_started:
        start_desktop()
        _desktop_started = True

    if _sunshine_proc is None:
        _sunshine_proc = start_sunshine()

    return create_proxy_app()


@app.local_entrypoint()
def main():
    """Local entrypoint for testing."""
    print("Sunshine WebRTC Modal Deployment")
    print("=" * 40)
    print("\nUsing pre-built Sunshine binary from local build")
    print("\nImage layers (cached independently):")
    print("  1. Ubuntu 24.04 + runtime deps")
    print("  2. Python proxy deps")
    print("  3. Pre-built Sunshine binary + libs")
    print("  4. Web assets")
    print("\nTo deploy:")
    print("  modal deploy modal_deploy/app.py")
    print("\nTo serve (development):")
    print("  modal serve modal_deploy/app.py")
    print("\nURL will be:")
    print("  https://<workspace>--sunshine-webrtc-sunshine-server.modal.run")
