"""
Sunshine WebRTC Proxy Server - GCP Deployment

A FastAPI-based reverse proxy that consolidates Sunshine's HTTP and WebSocket
servers onto a single port for cloud deployment.

Routes:
- /ws/signaling -> WebSocket proxy to internal signaling server
- /health -> Health check endpoint
- /api/status -> Connection status for auto-shutdown
- /api/server-logs -> Server-side logs
- /* -> HTTP proxy to internal HTTPS server
"""

import asyncio
import ssl
import os
import httpx
import websockets
from fastapi import FastAPI, WebSocket, Request, Response
from fastapi.middleware.cors import CORSMiddleware
from starlette.websockets import WebSocketDisconnect
import logging
from collections import deque
import threading
import time
import datetime

# Cloudflare TURN configuration
TURN_TOKEN_ID = os.environ.get("TURN_TOKEN_ID", "a4bc9d512adb0328945aa5175b91cf6c")
TURN_API_TOKEN = os.environ.get("TURN_API_TOKEN", "75ec637d125c89fca4dfac03926d348eb85942ce840b7ac312320774de21abe8")
TURN_CREDENTIAL_TTL = 86400  # 24 hours

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Internal Sunshine server configuration
SUNSHINE_HTTP_HOST = "127.0.0.1"
SUNSHINE_HTTP_PORT = 47990  # Config UI HTTPS port
SUNSHINE_WS_PORT = 47991    # WebSocket signaling port

# SSL context for connecting to internal Sunshine server (self-signed cert)
ssl_context = ssl.create_default_context()
ssl_context.check_hostname = False
ssl_context.verify_mode = ssl.CERT_NONE

# Connection tracking for auto-shutdown
active_connections = set()
last_activity = time.time()
connection_lock = threading.Lock()

# Server logs buffer
server_logs = deque(maxlen=200)
logs_lock = threading.Lock()

app = FastAPI(title="Sunshine WebRTC Proxy")

# Add CORS middleware for health check polling from play.sels.tech
app.add_middleware(
    CORSMiddleware,
    allow_origins=["https://play.sels.tech", "https://gaming.sels.tech"],
    allow_credentials=True,
    allow_methods=["GET", "POST", "OPTIONS"],
    allow_headers=["*"],
)


def add_server_log(level: str, message: str):
    """Add a log entry to the server logs buffer."""
    with logs_lock:
        server_logs.append({
            "time": datetime.datetime.now().isoformat(),
            "level": level,
            "message": message
        })


def update_activity():
    """Update last activity timestamp."""
    global last_activity
    with connection_lock:
        last_activity = time.time()


@app.websocket("/ws/signaling")
async def websocket_proxy(websocket: WebSocket):
    """
    Proxy WebSocket connections to the internal Sunshine signaling server.
    """
    await websocket.accept()
    connection_id = id(websocket)

    with connection_lock:
        active_connections.add(connection_id)

    update_activity()
    logger.info(f"Client WebSocket connected (id={connection_id}, total={len(active_connections)})")
    add_server_log("info", f"WebSocket client connected (total: {len(active_connections)})")

    ws_url = f"wss://{SUNSHINE_HTTP_HOST}:{SUNSHINE_WS_PORT}"

    try:
        async with websockets.connect(
            ws_url,
            ssl=ssl_context,
            ping_interval=20,
            ping_timeout=20
        ) as sunshine_ws:
            logger.info(f"Connected to Sunshine WebSocket at {ws_url}")

            async def client_to_sunshine():
                """Forward messages from client to Sunshine."""
                try:
                    while True:
                        data = await websocket.receive_text()
                        update_activity()
                        await sunshine_ws.send(data)
                except WebSocketDisconnect:
                    logger.info("Client disconnected")
                except Exception as e:
                    logger.error(f"Client->Sunshine error: {e}")

            async def sunshine_to_client():
                """Forward messages from Sunshine to client."""
                try:
                    async for message in sunshine_ws:
                        update_activity()
                        if isinstance(message, str):
                            await websocket.send_text(message)
                        else:
                            await websocket.send_bytes(message)
                except Exception as e:
                    logger.error(f"Sunshine->Client error: {e}")

            # Run both directions concurrently
            await asyncio.gather(
                client_to_sunshine(),
                sunshine_to_client(),
                return_exceptions=True
            )

    except Exception as e:
        logger.error(f"WebSocket proxy error: {e}")
        add_server_log("error", f"WebSocket proxy error: {e}")
        try:
            await websocket.close(code=1011, reason=str(e))
        except:
            pass
    finally:
        with connection_lock:
            active_connections.discard(connection_id)
        update_activity()
        logger.info(f"Client WebSocket disconnected (id={connection_id}, remaining={len(active_connections)})")
        add_server_log("info", f"WebSocket client disconnected (remaining: {len(active_connections)})")


@app.get("/health")
async def health_check():
    """Health check endpoint for load balancers."""
    return {"status": "healthy", "service": "sunshine-proxy"}


@app.get("/api/status")
async def get_status():
    """Get connection status for auto-shutdown decisions."""
    with connection_lock:
        conn_count = len(active_connections)
        idle_seconds = time.time() - last_activity

    return {
        "active_connections": conn_count,
        "idle_seconds": int(idle_seconds),
        "last_activity": datetime.datetime.fromtimestamp(last_activity).isoformat()
    }


@app.get("/api/server-logs")
async def get_server_logs():
    """Get server-side logs (Sunshine, GPU info, etc.)."""
    with logs_lock:
        logs = list(server_logs)
    return {"logs": logs}


@app.get("/api/turn-credentials")
async def get_turn_credentials():
    """
    Generate short-lived Cloudflare TURN credentials for WebRTC.
    Returns iceServers configuration to be passed to RTCPeerConnection.
    """
    if not TURN_TOKEN_ID or not TURN_API_TOKEN:
        logger.warning("TURN credentials not configured")
        # Return just STUN as fallback
        return {
            "iceServers": [
                {"urls": ["stun:stun.l.google.com:19302"]}
            ]
        }

    url = f"https://rtc.live.cloudflare.com/v1/turn/keys/{TURN_TOKEN_ID}/credentials/generate-ice-servers"
    headers = {
        "Authorization": f"Bearer {TURN_API_TOKEN}",
        "Content-Type": "application/json"
    }

    try:
        async with httpx.AsyncClient(timeout=10.0) as client:
            response = await client.post(
                url,
                headers=headers,
                json={"ttl": TURN_CREDENTIAL_TTL}
            )
            response.raise_for_status()
            data = response.json()
            logger.info("Generated Cloudflare TURN credentials")
            add_server_log("info", "Generated TURN credentials for client")
            return data
    except Exception as e:
        logger.error(f"Failed to generate TURN credentials: {e}")
        add_server_log("error", f"TURN credential generation failed: {e}")
        # Return STUN-only fallback
        return {
            "iceServers": [
                {"urls": ["stun:stun.l.google.com:19302"]}
            ]
        }


@app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"])
async def http_proxy(request: Request, path: str):
    """
    Proxy HTTP requests to the internal Sunshine HTTPS server.
    """
    update_activity()

    # Build the target URL
    url = f"https://{SUNSHINE_HTTP_HOST}:{SUNSHINE_HTTP_PORT}/{path}"
    if request.query_params:
        url += f"?{request.query_params}"

    # Get request body
    body = await request.body()

    # Forward headers (excluding hop-by-hop headers)
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

            # Filter response headers
            response_headers = dict(response.headers)
            # Remove hop-by-hop and encoding headers
            for header in hop_by_hop + ['content-encoding', 'content-length']:
                response_headers.pop(header, None)
            # Remove headers that block iframe embedding
            for header in ['x-frame-options', 'content-security-policy']:
                response_headers.pop(header, None)
            # Allow framing from sels.tech subdomains
            response_headers['content-security-policy'] = "frame-ancestors 'self' https://*.sels.tech"

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


if __name__ == "__main__":
    import uvicorn
    import os
    import multiprocessing

    # SSL certs from Let's Encrypt (DNS-01 challenge)
    ssl_cert = "/etc/letsencrypt/live/stream.sels.tech/fullchain.pem"
    ssl_key = "/etc/letsencrypt/live/stream.sels.tech/privkey.pem"

    def run_https():
        """Run HTTPS server on port 443."""
        uvicorn.run(
            app,
            host="0.0.0.0",
            port=443,
            ssl_certfile=ssl_cert,
            ssl_keyfile=ssl_key
        )

    def run_http_health():
        """Run HTTP health check server on port 8080."""
        uvicorn.run(app, host="0.0.0.0", port=8080)

    if os.path.exists(ssl_cert) and os.path.exists(ssl_key):
        # Run both HTTPS (443) and HTTP health check (8080) servers
        logger.info("Starting HTTPS server on port 443 with Let's Encrypt SSL")
        logger.info("Starting HTTP health check server on port 8080")

        # Start HTTP health check server in a separate process
        health_process = multiprocessing.Process(target=run_http_health)
        health_process.start()

        # Run HTTPS server in main process
        run_https()
    else:
        # Fallback to HTTP if no certs (for initial setup)
        logger.info("No SSL certs found, starting HTTP server on port 80")
        uvicorn.run(app, host="0.0.0.0", port=80)
