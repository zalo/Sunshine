"""
Modal Deployment for Sunshine Discord Activity

This deploys Sunshine as a Discord Activity with:
- Discord Embedded App SDK integration
- OAuth2 token exchange endpoint
- FFmpeg fMP4 streaming over WebSocket (Discord doesn't support WebRTC)
- MSE (Media Source Extensions) playback in browser
- GPU support for hardware video encoding

Usage:
    modal deploy modal_deploy/discord_app.py  # Production deployment
    modal serve modal_deploy/discord_app.py   # Development with hot reload

Environment Variables (set via Modal secrets):
    DISCORD_CLIENT_ID     - Your Discord application's client ID
    DISCORD_CLIENT_SECRET - Your Discord application's client secret
"""

import modal
import subprocess
import time
import os
import asyncio
import json
import threading
import queue
from collections import deque

# Define the Modal app
app = modal.App("sunshine-discord-activity")

# =============================================================================
# LAYER 1: Ubuntu 24.04 base with runtime dependencies and desktop environment
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
        "libicu74",
        "libevdev2",
        # Desktop environment (XFCE - lightweight)
        "xfce4", "xfce4-terminal", "dbus-x11",
        # Fonts
        "fonts-dejavu", "fonts-liberation", "fonts-noto-color-emoji",
        # Window manager utilities
        "xdotool", "wmctrl",
        # For Chrome
        "wget", "gnupg", "ca-certificates",
        # FFmpeg for fMP4 streaming (Discord doesn't support WebRTC)
        "ffmpeg",
    )
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
# LAYER 2: Python dependencies (FastAPI, Discord OAuth, WebSockets)
# =============================================================================
python_image = base_image.run_commands(
    "pip install --break-system-packages fastapi>=0.104.0 'uvicorn[standard]>=0.24.0' httpx>=0.25.0 websockets>=12.0"
)

# =============================================================================
# LAYER 3: Pre-built Sunshine binary and libraries
# =============================================================================
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
        "_deps/boost-src/",
        "libevdev-*/",
    ]
)

# =============================================================================
# LAYER 4: Discord Activity web assets
# =============================================================================
discord_activity_image = build_image.add_local_dir(
    "/home/selstad/Desktop/Sunshine/modal_deploy/discord_activity",
    remote_path="/opt/discord-activity",
    copy=True,
)

# =============================================================================
# LAYER 5: Sunshine web assets (for API endpoints)
# =============================================================================
final_image = discord_activity_image.add_local_dir(
    "/home/selstad/Desktop/Sunshine/src_assets/common/assets/web/public",
    remote_path="/opt/sunshine-web",
    copy=True,
)

# Volume for persistent state
sunshine_volume = modal.Volume.from_name("sunshine-discord-data", create_if_missing=True)


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
    """Start XFCE desktop environment."""
    env = os.environ.copy()
    env["DISPLAY"] = ":99"
    env["HOME"] = "/data"
    env["XDG_CONFIG_HOME"] = "/data/.config"
    env["XDG_DATA_HOME"] = "/data/.local/share"
    env["XDG_CACHE_HOME"] = "/data/.cache"
    env["DBUS_SESSION_BUS_ADDRESS"] = ""

    os.makedirs("/data/.config", exist_ok=True)
    os.makedirs("/data/.local/share", exist_ok=True)
    os.makedirs("/data/.cache", exist_ok=True)
    os.makedirs("/data/Desktop", exist_ok=True)

    # Chrome desktop shortcut
    chrome_desktop = """[Desktop Entry]
Version=1.0
Type=Application
Name=Google Chrome
Exec=/usr/bin/google-chrome-stable --no-sandbox --disable-gpu-sandbox %U
Icon=google-chrome
Terminal=false
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

    subprocess.Popen(
        ["startxfce4"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    time.sleep(3)
    print("XFCE desktop started")

    subprocess.Popen(
        ["xfce4-terminal", "--geometry=100x30+400+200"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    print("Terminal opened")


def start_sunshine():
    """Start the Sunshine server."""
    build_dir = "/opt/sunshine-build"
    lib_paths = [f"{build_dir}/lib_deps"]

    os.makedirs("/data/sunshine", exist_ok=True)

    config_path = "/data/sunshine/sunshine.conf"
    if not os.path.exists(config_path):
        with open(config_path, "w") as f:
            f.write("""# Sunshine Configuration
sunshine_name = Discord Activity
port = 47989
address_family = ipv4
webrtc_enabled = true
webrtc_stun_server = stun:stun.l.google.com:19302
min_log_level = info
file_apps = /opt/sunshine-web
""")

    env = os.environ.copy()
    env["HOME"] = "/data"
    env["XDG_CONFIG_HOME"] = "/data"
    env["DISPLAY"] = ":99"
    env["LD_LIBRARY_PATH"] = ":".join(lib_paths) + ":" + env.get("LD_LIBRARY_PATH", "")

    print(f"Starting Sunshine from {build_dir}")

    proc = subprocess.Popen(
        ["./sunshine", config_path],
        env=env,
        cwd=build_dir,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT
    )

    time.sleep(5)

    if proc.poll() is not None:
        output = proc.stdout.read().decode() if proc.stdout else "No output"
        raise RuntimeError(f"Sunshine failed to start: {output}")

    print("Sunshine started successfully!")
    return proc


def create_discord_activity_app():
    """Create the FastAPI application for Discord Activity."""
    import ssl
    import httpx
    from fastapi import FastAPI, WebSocket, Request, Response
    from fastapi.responses import HTMLResponse, FileResponse
    from starlette.websockets import WebSocketDisconnect
    import logging
    import asyncio
    import subprocess
    import struct

    logging.basicConfig(level=logging.INFO)
    logger = logging.getLogger(__name__)

    # =========================================================================
    # FFmpeg fMP4 Streaming Infrastructure
    # =========================================================================

    class FMP4Streamer:
        """Manages FFmpeg process and broadcasts fMP4 chunks to WebSocket clients."""

        def __init__(self):
            self.process = None
            self.clients = set()
            self.lock = asyncio.Lock()
            self.running = False
            self.init_segment = None  # Store the initialization segment (moov box)
            self.reader_task = None

        async def start(self):
            """Start FFmpeg capture process."""
            if self.running:
                return

            async with self.lock:
                if self.running:
                    return

                logger.info("Starting FFmpeg fMP4 capture...")

                # FFmpeg command to capture X11 and output fragmented MP4
                # -f x11grab: capture X11 display
                # -video_size: match Xvfb resolution
                # -framerate: target framerate
                # -i :99: display number
                # -c:v libx264: H.264 encoder (use h264_nvenc for GPU)
                # -preset ultrafast: lowest latency encoding
                # -tune zerolatency: optimize for streaming
                # -g 30: keyframe every 30 frames (1 second at 30fps)
                # -f mp4: output format
                # -movflags: fragmented MP4 flags for streaming
                # -frag_duration: fragment every 100ms for low latency

                cmd = [
                    "ffmpeg",
                    "-f", "x11grab",
                    "-video_size", "1920x1080",
                    "-framerate", "30",
                    "-i", ":99",
                    "-c:v", "libx264",
                    "-preset", "ultrafast",
                    "-tune", "zerolatency",
                    "-profile:v", "baseline",
                    "-level", "3.1",
                    "-pix_fmt", "yuv420p",
                    "-g", "30",
                    "-keyint_min", "30",
                    "-sc_threshold", "0",
                    "-b:v", "2M",
                    "-maxrate", "2M",
                    "-bufsize", "1M",
                    "-an",  # No audio for now
                    "-f", "mp4",
                    "-movflags", "frag_keyframe+empty_moov+default_base_moof+faststart",
                    "-frag_duration", "100000",  # 100ms fragments
                    "-reset_timestamps", "1",
                    "pipe:1"  # Output to stdout
                ]

                self.process = await asyncio.create_subprocess_exec(
                    *cmd,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE
                )

                self.running = True
                self.reader_task = asyncio.create_task(self._read_and_broadcast())

                # Log stderr in background
                asyncio.create_task(self._log_stderr())

                logger.info("FFmpeg started")

        async def _log_stderr(self):
            """Log FFmpeg stderr output."""
            try:
                while self.process and self.process.stderr:
                    line = await self.process.stderr.readline()
                    if not line:
                        break
                    logger.debug(f"FFmpeg: {line.decode().strip()}")
            except Exception as e:
                logger.error(f"FFmpeg stderr error: {e}")

        async def _read_and_broadcast(self):
            """Read fMP4 data from FFmpeg and broadcast to clients."""
            try:
                buffer = b""
                while self.running and self.process:
                    chunk = await self.process.stdout.read(65536)  # 64KB chunks
                    if not chunk:
                        logger.warning("FFmpeg stdout closed")
                        break

                    buffer += chunk

                    # Parse MP4 boxes and broadcast complete boxes
                    while len(buffer) >= 8:
                        # MP4 box header: 4 bytes size + 4 bytes type
                        box_size = struct.unpack(">I", buffer[:4])[0]
                        box_type = buffer[4:8].decode("latin-1", errors="replace")

                        if box_size == 0:
                            # Box extends to end of file
                            break
                        if box_size == 1:
                            # 64-bit size
                            if len(buffer) < 16:
                                break
                            box_size = struct.unpack(">Q", buffer[8:16])[0]

                        if len(buffer) < box_size:
                            # Need more data
                            break

                        # Extract complete box
                        box_data = buffer[:box_size]
                        buffer = buffer[box_size:]

                        # Store init segment (ftyp + moov)
                        if box_type in ("ftyp", "moov"):
                            if self.init_segment is None:
                                self.init_segment = box_data
                            else:
                                self.init_segment += box_data
                            logger.info(f"Captured {box_type} box ({len(box_data)} bytes)")

                        # Broadcast to all clients
                        await self._broadcast(box_data)

            except asyncio.CancelledError:
                logger.info("Reader task cancelled")
            except Exception as e:
                logger.error(f"Read/broadcast error: {e}")
            finally:
                self.running = False

        async def _broadcast(self, data):
            """Send data to all connected clients."""
            if not self.clients:
                return

            disconnected = set()
            for client in self.clients.copy():
                try:
                    await client.send_bytes(data)
                except Exception:
                    disconnected.add(client)

            self.clients -= disconnected

        async def add_client(self, websocket):
            """Add a new client and send init segment."""
            self.clients.add(websocket)
            logger.info(f"Client connected, total: {len(self.clients)}")

            # Send init segment if available
            if self.init_segment:
                try:
                    await websocket.send_bytes(self.init_segment)
                    logger.info(f"Sent init segment ({len(self.init_segment)} bytes)")
                except Exception as e:
                    logger.error(f"Failed to send init segment: {e}")

        def remove_client(self, websocket):
            """Remove a client."""
            self.clients.discard(websocket)
            logger.info(f"Client disconnected, total: {len(self.clients)}")

        async def stop(self):
            """Stop FFmpeg process."""
            self.running = False
            if self.reader_task:
                self.reader_task.cancel()
            if self.process:
                self.process.terminate()
                await self.process.wait()
                self.process = None
            logger.info("FFmpeg stopped")

    # Global streamer instance
    fmp4_streamer = FMP4Streamer()

    # Configuration from environment
    DISCORD_CLIENT_ID = os.environ.get("DISCORD_CLIENT_ID", "")
    DISCORD_CLIENT_SECRET = os.environ.get("DISCORD_CLIENT_SECRET", "")

    # Internal Sunshine server configuration
    SUNSHINE_HTTP_HOST = "127.0.0.1"
    SUNSHINE_HTTP_PORT = 47990
    SUNSHINE_WS_PORT = 47991

    # SSL context for self-signed cert
    ssl_context = ssl.create_default_context()
    ssl_context.check_hostname = False
    ssl_context.verify_mode = ssl.CERT_NONE

    web_app = FastAPI(title="Sunshine Discord Activity")

    # =========================================================================
    # Discord Activity Routes
    # =========================================================================

    # Cache busting version - increment to force refresh
    CACHE_VERSION = "v8-mse"

    @web_app.get("/")
    async def serve_activity():
        """Serve the Discord Activity HTML page."""
        html_path = "/opt/discord-activity/discord-activity.html"

        # Read and inject Discord Client ID
        with open(html_path, "r") as f:
            html_content = f.read()

        # Inject client ID as a global variable
        inject_script = f"""<script>window.DISCORD_CLIENT_ID = "{DISCORD_CLIENT_ID}";</script>"""
        html_content = html_content.replace("</head>", f"{inject_script}</head>")

        # Add cache busting to JS file reference
        html_content = html_content.replace(
            "discord-activity.js",
            f"discord-activity.js?{CACHE_VERSION}"
        )

        return HTMLResponse(
            content=html_content,
            headers={
                "Cache-Control": "no-cache, no-store, must-revalidate",
                "Pragma": "no-cache",
                "Expires": "0"
            }
        )

    @web_app.get("/discord-activity.js")
    async def serve_activity_js():
        """Serve the Discord Activity JavaScript."""
        with open("/opt/discord-activity/discord-activity.js", "r") as f:
            js_content = f.read()

        return Response(
            content=js_content,
            media_type="application/javascript",
            headers={
                "Cache-Control": "no-cache, no-store, must-revalidate",
                "Pragma": "no-cache",
                "Expires": "0"
            }
        )

    # =========================================================================
    # OAuth2 Token Exchange
    # =========================================================================

    @web_app.post("/api/token")
    async def exchange_token(request: Request):
        """Exchange Discord authorization code for access token."""
        try:
            body = await request.json()
            code = body.get("code")

            if not code:
                return Response(
                    content=json.dumps({"error": "Missing authorization code"}),
                    status_code=400,
                    media_type="application/json"
                )

            # Exchange code with Discord
            async with httpx.AsyncClient() as client:
                response = await client.post(
                    "https://discord.com/api/oauth2/token",
                    data={
                        "client_id": DISCORD_CLIENT_ID,
                        "client_secret": DISCORD_CLIENT_SECRET,
                        "grant_type": "authorization_code",
                        "code": code,
                    },
                    headers={
                        "Content-Type": "application/x-www-form-urlencoded"
                    }
                )

                if response.status_code != 200:
                    logger.error(f"Discord token exchange failed: {response.text}")
                    return Response(
                        content=json.dumps({"error": "Token exchange failed"}),
                        status_code=400,
                        media_type="application/json"
                    )

                token_data = response.json()
                return Response(
                    content=json.dumps({"access_token": token_data.get("access_token")}),
                    media_type="application/json"
                )

        except Exception as e:
            logger.error(f"Token exchange error: {e}")
            return Response(
                content=json.dumps({"error": str(e)}),
                status_code=500,
                media_type="application/json"
            )

    # =========================================================================
    # fMP4 Video Stream WebSocket (for Discord - no WebRTC support)
    # =========================================================================

    @web_app.websocket("/ws/stream")
    async def fmp4_stream_websocket(websocket: WebSocket):
        """Stream fMP4 video to clients via WebSocket."""
        await websocket.accept()
        logger.info("fMP4 stream client connected")

        # Start FFmpeg if not already running
        await fmp4_streamer.start()

        # Add client to broadcast list
        await fmp4_streamer.add_client(websocket)

        try:
            # Keep connection alive, handle any client messages
            while True:
                try:
                    # Client might send control messages (quality, etc.)
                    data = await asyncio.wait_for(
                        websocket.receive_text(),
                        timeout=30.0
                    )
                    # Handle client messages if needed
                    logger.debug(f"Stream client message: {data}")
                except asyncio.TimeoutError:
                    # Send ping to keep connection alive
                    await websocket.send_text('{"type":"ping"}')
                except WebSocketDisconnect:
                    break
        except Exception as e:
            logger.error(f"fMP4 stream error: {e}")
        finally:
            fmp4_streamer.remove_client(websocket)

    # =========================================================================
    # Input Handling WebSocket (keyboard/mouse via xdotool)
    # =========================================================================

    @web_app.websocket("/ws/input")
    async def input_websocket(websocket: WebSocket):
        """Handle keyboard and mouse input via xdotool."""
        await websocket.accept()
        logger.info("Input client connected")

        env = os.environ.copy()
        env["DISPLAY"] = ":99"

        try:
            while True:
                data = await websocket.receive_text()
                msg = json.loads(data)
                msg_type = msg.get("type")

                if msg_type == "mousemove":
                    # Absolute mouse position
                    x = msg.get("x", 0)
                    y = msg.get("y", 0)
                    subprocess.run(
                        ["xdotool", "mousemove", str(x), str(y)],
                        env=env,
                        capture_output=True
                    )

                elif msg_type == "mousedown":
                    button = msg.get("button", 1)
                    subprocess.run(
                        ["xdotool", "mousedown", str(button)],
                        env=env,
                        capture_output=True
                    )

                elif msg_type == "mouseup":
                    button = msg.get("button", 1)
                    subprocess.run(
                        ["xdotool", "mouseup", str(button)],
                        env=env,
                        capture_output=True
                    )

                elif msg_type == "click":
                    button = msg.get("button", 1)
                    subprocess.run(
                        ["xdotool", "click", str(button)],
                        env=env,
                        capture_output=True
                    )

                elif msg_type == "scroll":
                    dx = msg.get("dx", 0)
                    dy = msg.get("dy", 0)
                    # xdotool uses button 4/5 for scroll
                    if dy > 0:
                        for _ in range(min(abs(dy) // 30, 5)):
                            subprocess.run(["xdotool", "click", "4"], env=env, capture_output=True)
                    elif dy < 0:
                        for _ in range(min(abs(dy) // 30, 5)):
                            subprocess.run(["xdotool", "click", "5"], env=env, capture_output=True)

                elif msg_type == "keydown":
                    key = msg.get("key", "")
                    if key:
                        subprocess.run(
                            ["xdotool", "keydown", key],
                            env=env,
                            capture_output=True
                        )

                elif msg_type == "keyup":
                    key = msg.get("key", "")
                    if key:
                        subprocess.run(
                            ["xdotool", "keyup", key],
                            env=env,
                            capture_output=True
                        )

                elif msg_type == "key":
                    # Key event from client with pressed state
                    key = msg.get("key", "")
                    pressed = msg.get("pressed", True)
                    if key:
                        # Map browser keys to xdotool keys
                        key_map = {
                            "ArrowLeft": "Left", "ArrowRight": "Right",
                            "ArrowUp": "Up", "ArrowDown": "Down",
                            "Backspace": "BackSpace", "Enter": "Return",
                            "Escape": "Escape", "Tab": "Tab",
                            " ": "space", "Control": "ctrl",
                            "Alt": "alt", "Shift": "shift",
                            "Meta": "super", "CapsLock": "Caps_Lock"
                        }
                        xdo_key = key_map.get(key, key)

                        if pressed:
                            subprocess.run(
                                ["xdotool", "keydown", xdo_key],
                                env=env,
                                capture_output=True
                            )
                        else:
                            subprocess.run(
                                ["xdotool", "keyup", xdo_key],
                                env=env,
                                capture_output=True
                            )

                elif msg_type == "mousebutton":
                    # Mouse button event from client
                    button = msg.get("button", 1)
                    pressed = msg.get("pressed", False)
                    if pressed:
                        subprocess.run(
                            ["xdotool", "mousedown", str(button)],
                            env=env,
                            capture_output=True
                        )
                    else:
                        subprocess.run(
                            ["xdotool", "mouseup", str(button)],
                            env=env,
                            capture_output=True
                        )

                elif msg_type == "type":
                    # Type text
                    text = msg.get("text", "")
                    if text:
                        subprocess.run(
                            ["xdotool", "type", "--", text],
                            env=env,
                            capture_output=True
                        )

        except WebSocketDisconnect:
            logger.info("Input client disconnected")
        except Exception as e:
            logger.error(f"Input handling error: {e}")

    # =========================================================================
    # Legacy WebSocket Signaling Proxy (for non-Discord clients)
    # =========================================================================

    @web_app.websocket("/ws/signaling")
    async def websocket_proxy(websocket: WebSocket):
        """Proxy WebSocket connections to internal signaling server (legacy)."""
        await websocket.accept()
        logger.info("Legacy signaling client connected")

        ws_url = f"wss://{SUNSHINE_HTTP_HOST}:{SUNSHINE_WS_PORT}"

        try:
            import websockets
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

    # =========================================================================
    # Health Check & HTTP Proxy
    # =========================================================================

    @web_app.get("/health")
    async def health_check():
        """Health check endpoint."""
        return {"status": "healthy", "service": "sunshine-discord-activity"}

    @web_app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"])
    async def http_proxy(request: Request, path: str):
        """Proxy HTTP requests to internal Sunshine server."""
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
    gpu="L4",
    volumes={"/data": sunshine_volume},
    timeout=86400,
    scaledown_window=300,
    secrets=[modal.Secret.from_name("discord-sunshine-secrets")],
)
@modal.concurrent(max_inputs=100)
@modal.asgi_app()
def discord_activity_server():
    """
    Discord Activity server entry point.

    Starts:
    1. Xvfb virtual display
    2. XFCE desktop environment
    3. Sunshine streaming server
    4. FastAPI app (OAuth + WebSocket proxy)
    """
    global _xvfb_started, _desktop_started, _sunshine_proc

    if not _xvfb_started:
        start_xvfb()
        _xvfb_started = True

    if not _desktop_started:
        start_desktop()
        _desktop_started = True

    if _sunshine_proc is None:
        _sunshine_proc = start_sunshine()

    return create_discord_activity_app()


@app.local_entrypoint()
def main():
    """Local entrypoint for testing."""
    print("Sunshine Discord Activity Deployment")
    print("=" * 45)
    print("\nThis deploys Sunshine as a Discord Activity")
    print("\nBefore deploying, create a Modal secret:")
    print("  modal secret create discord-sunshine-secrets \\")
    print("    DISCORD_CLIENT_ID=your_client_id \\")
    print("    DISCORD_CLIENT_SECRET=your_client_secret")
    print("\nImage layers (cached independently):")
    print("  1. Ubuntu 24.04 + runtime deps + desktop")
    print("  2. Python deps (FastAPI, httpx, websockets)")
    print("  3. Pre-built Sunshine binary + libs")
    print("  4. Discord Activity web assets")
    print("  5. Sunshine web assets")
    print("\nTo deploy:")
    print("  modal deploy modal_deploy/discord_app.py")
    print("\nTo serve (development):")
    print("  modal serve modal_deploy/discord_app.py")
    print("\nDiscord Developer Portal Setup:")
    print("  1. Create an app at https://discord.com/developers/applications")
    print("  2. Enable 'Activities' in the app settings")
    print("  3. Set the Activity URL Mapping to your Modal URL:")
    print("     https://<workspace>--sunshine-discord-activity-discord-activity-server.modal.run")
    print("  4. Add OAuth2 redirect: https://discord.com/api/oauth2/authorize")
