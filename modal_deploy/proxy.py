"""
Sunshine WebRTC Proxy Server

A FastAPI-based reverse proxy that consolidates Sunshine's HTTP and WebSocket
servers onto a single port for cloud deployment (Modal, etc.).

Routes:
- /ws/signaling -> WebSocket proxy to internal signaling server
- /* -> HTTP proxy to internal HTTPS server
"""

import asyncio
import ssl
import httpx
import websockets
from fastapi import FastAPI, WebSocket, Request, Response
from fastapi.responses import StreamingResponse
from starlette.websockets import WebSocketDisconnect
import logging

# Configure logging
logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

# Internal Sunshine server configuration
# These run inside the same container
SUNSHINE_HTTP_HOST = "127.0.0.1"
SUNSHINE_HTTP_PORT = 47990  # Config UI HTTPS port
SUNSHINE_WS_PORT = 47991    # WebSocket signaling port

# SSL context for connecting to internal Sunshine server (self-signed cert)
ssl_context = ssl.create_default_context()
ssl_context.check_hostname = False
ssl_context.verify_mode = ssl.CERT_NONE

app = FastAPI(title="Sunshine WebRTC Proxy")


@app.websocket("/ws/signaling")
async def websocket_proxy(websocket: WebSocket):
    """
    Proxy WebSocket connections to the internal Sunshine signaling server.
    """
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
            logger.info(f"Connected to Sunshine WebSocket at {ws_url}")

            async def client_to_sunshine():
                """Forward messages from client to Sunshine."""
                try:
                    while True:
                        data = await websocket.receive_text()
                        await sunshine_ws.send(data)
                except WebSocketDisconnect:
                    logger.info("Client disconnected")
                except Exception as e:
                    logger.error(f"Client->Sunshine error: {e}")

            async def sunshine_to_client():
                """Forward messages from Sunshine to client."""
                try:
                    async for message in sunshine_ws:
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
        try:
            await websocket.close(code=1011, reason=str(e))
        except:
            pass


@app.api_route("/{path:path}", methods=["GET", "POST", "PUT", "DELETE", "PATCH", "OPTIONS"])
async def http_proxy(request: Request, path: str):
    """
    Proxy HTTP requests to the internal Sunshine HTTPS server.
    """
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


@app.get("/health")
async def health_check():
    """Health check endpoint for load balancers."""
    return {"status": "healthy", "service": "sunshine-proxy"}


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
