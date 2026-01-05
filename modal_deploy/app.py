"""
Modal Deployment for Sunshine WebRTC Streaming Server

This deploys Sunshine on Modal with:
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
import signal
import asyncio

# Define the Modal app
app = modal.App("sunshine-webrtc")

# Build the Sunshine image with all dependencies
sunshine_image = (
    modal.Image.from_registry("ubuntu:24.04")
    # System dependencies
    .apt_install(
        # Build tools
        "build-essential", "cmake", "ninja-build", "git", "pkg-config",
        # Sunshine dependencies
        "libssl-dev", "libcurl4-openssl-dev", "libminiupnpc-dev",
        "libboost-all-dev", "libopus-dev", "libpulse-dev",
        # X11 and display
        "xvfb", "x11-utils", "libx11-dev", "libxcb1-dev",
        "libxfixes-dev", "libxrandr-dev", "libxtst-dev",
        "libxcb-shm0-dev", "libxcb-xfixes0-dev",
        # Video encoding
        "libva-dev", "libdrm-dev", "libgbm-dev",
        # Audio
        "libasound2-dev",
        # Other
        "libcap-dev", "libnuma-dev",
        # Node.js for web UI build
        "nodejs", "npm",
    )
    # Install Python dependencies for proxy
    .pip_install(
        "fastapi>=0.104.0",
        "uvicorn[standard]>=0.24.0",
        "httpx>=0.25.0",
        "websockets>=12.0",
    )
    # Set environment variables
    .env({
        "DISPLAY": ":99",
    })
)

# Volume for persistent state (config, credentials, etc.)
sunshine_volume = modal.Volume.from_name("sunshine-data", create_if_missing=True)


def start_xvfb():
    """Start Xvfb virtual display."""
    subprocess.Popen(
        ["Xvfb", ":99", "-screen", "0", "1920x1080x24"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL
    )
    time.sleep(2)
    print("Xvfb started on display :99")


def start_sunshine(sunshine_path: str):
    """Start the Sunshine server."""
    os.chdir(sunshine_path)

    # Create data directory
    os.makedirs("/data/sunshine", exist_ok=True)

    # Start Sunshine
    env = os.environ.copy()
    env["HOME"] = "/data"
    env["XDG_CONFIG_HOME"] = "/data"

    proc = subprocess.Popen(
        [f"{sunshine_path}/build/sunshine", "--config", "/data/sunshine/sunshine.conf"],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT
    )

    # Wait for startup
    time.sleep(5)

    if proc.poll() is not None:
        output = proc.stdout.read().decode()
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
                logger.info(f"Connected to Sunshine WebSocket")

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

    @web_app.get("/health")
    async def health_check():
        return {"status": "healthy", "service": "sunshine-proxy"}

    return web_app


# Global process handle
_sunshine_proc = None


@app.function(
    image=sunshine_image,
    gpu="L4",  # Good cost/performance for video encoding
    volumes={"/data": sunshine_volume},
    timeout=86400,  # 24 hours max session
    container_idle_timeout=300,  # 5 min idle before shutdown
)
@modal.concurrent(max_inputs=100)  # Handle many WebSocket connections
@modal.asgi_app()
def sunshine_server():
    """
    Main Sunshine server entry point.

    Starts:
    1. Xvfb virtual display
    2. Sunshine streaming server (internal ports 47990, 47991)
    3. FastAPI proxy (exposed via ASGI)
    """
    global _sunshine_proc

    # Start services on first request
    if _sunshine_proc is None:
        start_xvfb()

        # For now, assume Sunshine is pre-built and available
        # In production, you'd mount the built binary or build it in the image
        sunshine_path = "/sunshine"
        if os.path.exists(f"{sunshine_path}/build/sunshine"):
            _sunshine_proc = start_sunshine(sunshine_path)
        else:
            print("Warning: Sunshine not built. Run 'ninja -C /sunshine/build' first.")

    return create_proxy_app()


@app.local_entrypoint()
def main():
    """Local entrypoint for testing."""
    print("Sunshine WebRTC Modal Deployment")
    print("=" * 40)
    print("\nTo deploy:")
    print("  modal deploy modal_deploy/app.py")
    print("\nTo serve (development):")
    print("  modal serve modal_deploy/app.py")
    print("\nURL will be:")
    print("  https://<workspace>--sunshine-webrtc-sunshine-server.modal.run")
