/**
 * Cloudflare Worker for Sunshine WebRTC Streaming on GCP
 *
 * Features:
 * - Auto-starts GCP VM when users visit the page
 * - Embeds Sunshine stream via iframe once VM is ready
 * - Monitors page views and auto-shutdowns VM after 5 minutes of no clients
 *
 * Environment Variables Required:
 * - GCP_PROJECT: Google Cloud project ID
 * - GCP_ZONE: Zone where VM is located (e.g., us-west1-a)
 * - GCP_INSTANCE: VM instance name
 * - GCP_SERVICE_ACCOUNT_JSON: Service account JSON key (base64 encoded)
 * - SUNSHINE_TUNNEL_URL: Direct URL for Sunshine stream (e.g., https://stream.sels.tech)
 */

export interface Env {
  GCP_PROJECT: string;
  GCP_ZONE: string;
  GCP_INSTANCE: string;
  GCP_SERVICE_ACCOUNT_JSON: string;
  SUNSHINE_TUNNEL_URL: string;
  // KV namespace for state management
  SUNSHINE_STATE: KVNamespace;
}

// Constants
const IDLE_TIMEOUT_SECONDS = 60 * 60; // 1 hour (effectively disabled for testing)
const HEARTBEAT_INTERVAL_SECONDS = 30; // Client heartbeat every 30s

/**
 * Get Google OAuth 2.0 access token using service account credentials
 */
async function getGoogleAccessToken(serviceAccountJson: string): Promise<string> {
  const serviceAccount = JSON.parse(atob(serviceAccountJson));

  // Create JWT header
  const header = {
    alg: "RS256",
    typ: "JWT",
  };

  // Create JWT payload
  const now = Math.floor(Date.now() / 1000);
  const payload = {
    iss: serviceAccount.client_email,
    scope: "https://www.googleapis.com/auth/compute",
    aud: "https://oauth2.googleapis.com/token",
    iat: now,
    exp: now + 3600,
  };

  // Base64url encode
  const base64url = (obj: object) =>
    btoa(JSON.stringify(obj))
      .replace(/\+/g, "-")
      .replace(/\//g, "_")
      .replace(/=+$/, "");

  const headerB64 = base64url(header);
  const payloadB64 = base64url(payload);
  const signatureInput = `${headerB64}.${payloadB64}`;

  // Import private key and sign
  const privateKey = await crypto.subtle.importKey(
    "pkcs8",
    pemToArrayBuffer(serviceAccount.private_key),
    { name: "RSASSA-PKCS1-v1_5", hash: "SHA-256" },
    false,
    ["sign"]
  );

  const signature = await crypto.subtle.sign(
    "RSASSA-PKCS1-v1_5",
    privateKey,
    new TextEncoder().encode(signatureInput)
  );

  const signatureB64 = btoa(String.fromCharCode(...new Uint8Array(signature)))
    .replace(/\+/g, "-")
    .replace(/\//g, "_")
    .replace(/=+$/, "");

  const jwt = `${signatureInput}.${signatureB64}`;

  // Exchange JWT for access token
  const tokenResponse = await fetch("https://oauth2.googleapis.com/token", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=${jwt}`,
  });

  const tokenData = (await tokenResponse.json()) as { access_token: string };
  return tokenData.access_token;
}

/**
 * Convert PEM to ArrayBuffer
 */
function pemToArrayBuffer(pem: string): ArrayBuffer {
  const b64 = pem
    .replace(/-----BEGIN PRIVATE KEY-----/, "")
    .replace(/-----END PRIVATE KEY-----/, "")
    .replace(/\s/g, "");
  const binary = atob(b64);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) {
    bytes[i] = binary.charCodeAt(i);
  }
  return bytes.buffer;
}

/**
 * VM instance info from GCP API
 */
interface VMInstanceInfo {
  status: string;
  externalIP: string | null;
}

/**
 * Get VM status and external IP from GCP
 */
async function getVMInfo(env: Env): Promise<VMInstanceInfo> {
  const accessToken = await getGoogleAccessToken(env.GCP_SERVICE_ACCOUNT_JSON);
  const url = `https://compute.googleapis.com/compute/v1/projects/${env.GCP_PROJECT}/zones/${env.GCP_ZONE}/instances/${env.GCP_INSTANCE}`;

  const response = await fetch(url, {
    headers: { Authorization: `Bearer ${accessToken}` },
  });

  if (!response.ok) {
    throw new Error(`Failed to get VM info: ${response.statusText}`);
  }

  const data = (await response.json()) as {
    status: string;
    networkInterfaces?: Array<{
      accessConfigs?: Array<{ natIP?: string }>;
    }>;
  };

  // Extract external IP from network interfaces
  let externalIP: string | null = null;
  if (data.networkInterfaces?.[0]?.accessConfigs?.[0]?.natIP) {
    externalIP = data.networkInterfaces[0].accessConfigs[0].natIP;
  }

  return {
    status: data.status, // RUNNING, TERMINATED, STAGING, etc.
    externalIP,
  };
}

/**
 * Get VM status from GCP (legacy wrapper)
 */
async function getVMStatus(env: Env): Promise<string> {
  const info = await getVMInfo(env);
  return info.status;
}

/**
 * Start the GCP VM
 */
async function startVM(env: Env): Promise<void> {
  const accessToken = await getGoogleAccessToken(env.GCP_SERVICE_ACCOUNT_JSON);
  const url = `https://compute.googleapis.com/compute/v1/projects/${env.GCP_PROJECT}/zones/${env.GCP_ZONE}/instances/${env.GCP_INSTANCE}/start`;

  const response = await fetch(url, {
    method: "POST",
    headers: { Authorization: `Bearer ${accessToken}` },
  });

  if (!response.ok) {
    const error = await response.text();
    throw new Error(`Failed to start VM: ${error}`);
  }

  // Store start time in KV
  await env.SUNSHINE_STATE.put("vm_start_time", Date.now().toString());
}

/**
 * Stop the GCP VM
 */
async function stopVM(env: Env): Promise<void> {
  const accessToken = await getGoogleAccessToken(env.GCP_SERVICE_ACCOUNT_JSON);
  const url = `https://compute.googleapis.com/compute/v1/projects/${env.GCP_PROJECT}/zones/${env.GCP_ZONE}/instances/${env.GCP_INSTANCE}/stop`;

  const response = await fetch(url, {
    method: "POST",
    headers: { Authorization: `Bearer ${accessToken}` },
  });

  if (!response.ok) {
    const error = await response.text();
    throw new Error(`Failed to stop VM: ${error}`);
  }

  await env.SUNSHINE_STATE.delete("vm_start_time");
  await env.SUNSHINE_STATE.delete("last_heartbeat");
}

/**
 * Check Sunshine status via HTTP on port 8080 (bypasses Cloudflare same-zone restriction)
 */
async function checkSunshineStatus(vmIP: string): Promise<{ ready: boolean; active: number; idle: number }> {
  try {
    // Use HTTP on port 8080 - the proxy runs a health check server there
    // This bypasses Cloudflare's restriction on fetching from same-zone domains
    const response = await fetch(`http://${vmIP}:8080/api/status`, {
      cf: { cacheTtl: 0 },
    });

    if (!response.ok) {
      return { ready: false, active: 0, idle: 0 };
    }

    const data = (await response.json()) as {
      active_connections: number;
      idle_seconds: number;
    };
    return {
      ready: true,
      active: data.active_connections,
      idle: data.idle_seconds,
    };
  } catch {
    return { ready: false, active: 0, idle: 0 };
  }
}

/**
 * Record client heartbeat
 */
async function recordHeartbeat(env: Env): Promise<void> {
  await env.SUNSHINE_STATE.put("last_heartbeat", Date.now().toString());
}

/**
 * Get seconds since last heartbeat
 */
async function getSecondsSinceLastHeartbeat(env: Env): Promise<number> {
  const lastHeartbeat = await env.SUNSHINE_STATE.get("last_heartbeat");
  if (!lastHeartbeat) {
    return Infinity;
  }
  return Math.floor((Date.now() - parseInt(lastHeartbeat)) / 1000);
}

/**
 * Generate the streaming page HTML with embedded iframe and health monitoring
 */
function generateStreamingPage(heartbeatInterval: number): string {
  const IDLE_SHUTDOWN_MS = 5 * 60 * 1000; // 5 minutes
  const HEALTH_CHECK_INTERVAL_MS = 10000; // Check health every 10 seconds

  return `<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Sunshine Cloud Gaming</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        html, body {
            width: 100%;
            height: 100%;
            overflow: hidden;
            background: #0a0a0f;
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            color: #fff;
        }
        .stream-container {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            z-index: 1;
        }
        .stream-iframe {
            width: 100%;
            height: 100%;
            border: none;
        }
        .loading-screen {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 50%, #0f3460 100%);
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            gap: 2rem;
            z-index: 100;
            transition: opacity 0.5s ease;
        }
        .loading-screen.hidden {
            opacity: 0;
            pointer-events: none;
        }
        .logo {
            font-size: 3rem;
            font-weight: 700;
            background: linear-gradient(90deg, #e94560, #ff6b6b);
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
        }
        .spinner-container {
            display: flex;
            flex-direction: column;
            align-items: center;
            gap: 1.5rem;
        }
        .spinner {
            width: 60px;
            height: 60px;
            border: 4px solid rgba(255, 255, 255, 0.1);
            border-top-color: #e94560;
            border-radius: 50%;
            animation: spin 1s linear infinite;
        }
        @keyframes spin {
            to { transform: rotate(360deg); }
        }
        .status-text {
            font-size: 1.2rem;
            color: rgba(255, 255, 255, 0.8);
            text-align: center;
            max-width: 400px;
        }
        .status-detail {
            font-size: 0.9rem;
            color: rgba(255, 255, 255, 0.5);
            margin-top: 0.5rem;
        }
        .progress-bar {
            width: 300px;
            height: 4px;
            background: rgba(255, 255, 255, 0.1);
            border-radius: 2px;
            overflow: hidden;
        }
        .progress-fill {
            height: 100%;
            background: linear-gradient(90deg, #e94560, #ff6b6b);
            width: 0%;
            transition: width 0.3s ease;
        }
        .status-overlay {
            position: fixed;
            top: 10px;
            right: 10px;
            background: rgba(0, 0, 0, 0.7);
            padding: 8px 12px;
            border-radius: 6px;
            font-size: 12px;
            z-index: 200;
            display: none;
            cursor: pointer;
        }
        .status-overlay.visible { display: block; }
        .status-overlay.healthy { border-left: 3px solid #4ade80; }
        .status-overlay.warning { border-left: 3px solid #fbbf24; }
        .status-overlay.error { border-left: 3px solid #f87171; }
        .shutdown-warning {
            position: fixed;
            top: 50%;
            left: 50%;
            transform: translate(-50%, -50%);
            background: rgba(0, 0, 0, 0.9);
            padding: 2rem;
            border-radius: 12px;
            text-align: center;
            z-index: 300;
            display: none;
            border: 2px solid #f87171;
        }
        .shutdown-warning.visible { display: block; }
        .shutdown-warning h2 { color: #f87171; margin-bottom: 1rem; }
        .shutdown-warning p { margin-bottom: 1rem; color: rgba(255,255,255,0.8); }
        .shutdown-warning .countdown { font-size: 2rem; color: #fbbf24; }
        .error-screen {
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: linear-gradient(135deg, #1a1a2e 0%, #16213e 50%, #0f3460 100%);
            display: none;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            gap: 1.5rem;
            z-index: 101;
        }
        .error-screen.visible { display: flex; }
        .error-icon { font-size: 4rem; }
        .error-message { font-size: 1.2rem; color: #f87171; }
        .retry-btn {
            padding: 1rem 2rem;
            font-size: 1rem;
            font-weight: 600;
            background: linear-gradient(90deg, #e94560, #ff6b6b);
            color: white;
            border: none;
            border-radius: 0.5rem;
            cursor: pointer;
            transition: transform 0.2s;
        }
        .retry-btn:hover { transform: translateY(-2px); }
    </style>
</head>
<body>
    <div class="stream-container" id="streamContainer" style="display: none;">
        <iframe id="streamFrame" class="stream-iframe" allow="autoplay; fullscreen; gamepad; pointer-lock; clipboard-write"></iframe>
    </div>

    <div class="status-overlay" id="statusOverlay">
        <span id="overlayText">Checking...</span>
    </div>

    <div class="shutdown-warning" id="shutdownWarning">
        <h2>Connection Lost</h2>
        <p>Server will shut down due to inactivity</p>
        <div class="countdown" id="shutdownCountdown">5:00</div>
        <p style="font-size: 0.9rem; color: rgba(255,255,255,0.5);">Reconnecting will cancel shutdown</p>
    </div>

    <div class="loading-screen" id="loadingScreen">
        <div class="logo">Sunshine</div>
        <div class="spinner-container">
            <div class="spinner"></div>
            <div class="status-text" id="statusText">Initializing...</div>
            <div class="status-detail" id="statusDetail"></div>
        </div>
        <div class="progress-bar">
            <div class="progress-fill" id="progressFill"></div>
        </div>
    </div>

    <div class="error-screen" id="errorScreen">
        <div class="error-icon">⚠️</div>
        <div class="error-message" id="errorMessage">An error occurred</div>
        <button class="retry-btn" onclick="location.reload()">Retry</button>
    </div>

    <script>
        const heartbeatInterval = ${heartbeatInterval};
        const IDLE_SHUTDOWN_MS = ${IDLE_SHUTDOWN_MS};
        const HEALTH_CHECK_INTERVAL_MS = ${HEALTH_CHECK_INTERVAL_MS};

        let startTime = Date.now();
        let vmStarted = false;
        let vmIP = null;
        let lastHealthCheckSuccess = Date.now();
        let healthCheckInterval = null;
        let isStreaming = false;
        let shutdownInitiated = false;

        const statusText = document.getElementById('statusText');
        const statusDetail = document.getElementById('statusDetail');
        const progressFill = document.getElementById('progressFill');
        const loadingScreen = document.getElementById('loadingScreen');
        const errorScreen = document.getElementById('errorScreen');
        const errorMessage = document.getElementById('errorMessage');
        const streamContainer = document.getElementById('streamContainer');
        const streamFrame = document.getElementById('streamFrame');
        const statusOverlay = document.getElementById('statusOverlay');
        const overlayText = document.getElementById('overlayText');
        const shutdownWarning = document.getElementById('shutdownWarning');
        const shutdownCountdown = document.getElementById('shutdownCountdown');

        function log(message, level = 'info') {
            const now = new Date();
            const time = now.toLocaleTimeString('en-US', { hour12: false });
            console.log('[' + level.toUpperCase() + '] ' + time + ' - ' + message);
        }

        function showError(msg) {
            log('ERROR: ' + msg, 'error');
            errorMessage.textContent = msg;
            errorScreen.classList.add('visible');
        }

        function updateProgress(percent, text, detail = '') {
            progressFill.style.width = percent + '%';
            statusText.textContent = text;
            statusDetail.textContent = detail;
        }

        function updateStatusOverlay(status, message) {
            statusOverlay.className = 'status-overlay visible ' + status;
            overlayText.textContent = message;
        }

        async function sendHeartbeat() {
            try {
                await fetch('/api/heartbeat', { method: 'POST' });
            } catch (e) {
                console.error('Heartbeat failed:', e);
            }
        }

        async function getVMInfo() {
            const response = await fetch('/api/vm-info');
            return await response.json();
        }

        async function startVM() {
            log('Starting VM...', 'info');
            const response = await fetch('/api/start-vm', { method: 'POST' });
            if (!response.ok) throw new Error('Failed to start VM');
            return await response.json();
        }

        async function stopVM() {
            log('Stopping VM...', 'warn');
            try {
                await fetch('/api/stop-vm', { method: 'POST' });
            } catch (e) {
                console.error('Failed to stop VM:', e);
            }
        }

        async function checkHealth() {
            try {
                const response = await fetch('https://stream.sels.tech/health', {
                    method: 'GET',
                    mode: 'cors'
                });
                if (response.ok) {
                    const data = await response.json();
                    if (data.status === 'healthy') {
                        lastHealthCheckSuccess = Date.now();
                        await sendHeartbeat();
                        return true;
                    }
                }
            } catch (e) {
                // Health check failed
            }
            return false;
        }

        async function monitorHealth() {
            const healthy = await checkHealth();
            const timeSinceLastSuccess = Date.now() - lastHealthCheckSuccess;
            const timeRemaining = Math.max(0, IDLE_SHUTDOWN_MS - timeSinceLastSuccess);
            const minutesRemaining = Math.floor(timeRemaining / 60000);
            const secondsRemaining = Math.floor((timeRemaining % 60000) / 1000);

            if (healthy) {
                updateStatusOverlay('healthy', 'Connected');
                shutdownWarning.classList.remove('visible');
                shutdownInitiated = false;
            } else if (timeSinceLastSuccess > 60000) {
                // Show warning after 1 minute of failures
                updateStatusOverlay('warning', 'Connection issues - ' + minutesRemaining + ':' + String(secondsRemaining).padStart(2, '0'));

                if (timeSinceLastSuccess > 120000) {
                    // Show shutdown warning after 2 minutes
                    shutdownWarning.classList.add('visible');
                    shutdownCountdown.textContent = minutesRemaining + ':' + String(secondsRemaining).padStart(2, '0');
                }
            }

            // Auto-shutdown after 5 minutes of no successful health checks
            if (timeRemaining <= 0 && !shutdownInitiated) {
                shutdownInitiated = true;
                log('Initiating auto-shutdown due to inactivity', 'warn');
                updateStatusOverlay('error', 'Shutting down...');
                await stopVM();

                // Show final message
                shutdownWarning.classList.remove('visible');
                showError('Server shut down due to inactivity. Refresh to restart.');

                if (healthCheckInterval) {
                    clearInterval(healthCheckInterval);
                }
            }
        }

        function showStream() {
            isStreaming = true;
            streamFrame.src = 'https://stream.sels.tech/play';
            streamContainer.style.display = 'block';
            loadingScreen.classList.add('hidden');
            statusOverlay.classList.add('visible');

            // Start continuous health monitoring
            lastHealthCheckSuccess = Date.now();
            healthCheckInterval = setInterval(monitorHealth, HEALTH_CHECK_INTERVAL_MS);

            log('Stream started, health monitoring active', 'success');
        }

        async function initialize() {
            log('Initialization started', 'info');

            try {
                await sendHeartbeat();

                updateProgress(10, 'Checking server status...');
                let vmInfo = await getVMInfo();
                log('VM status: ' + vmInfo.status, 'info');

                if (vmInfo.status === 'TERMINATED' || vmInfo.status === 'STOPPED') {
                    updateProgress(20, 'Starting cloud server...', 'This may take 1-2 minutes');
                    await startVM();
                    vmStarted = true;
                } else if (vmInfo.status === 'RUNNING') {
                    log('VM already running', 'success');
                }

                // Wait for VM to be running
                let pollCount = 0;
                while (vmInfo.status !== 'RUNNING' || !vmInfo.externalIP) {
                    pollCount++;
                    const elapsed = Math.floor((Date.now() - startTime) / 1000);
                    updateProgress(Math.min(20 + elapsed, 60), 'Starting cloud server...', 'Elapsed: ' + elapsed + 's');

                    await new Promise(r => setTimeout(r, 3000));
                    vmInfo = await getVMInfo();
                    await sendHeartbeat();

                    if (elapsed > 300) {
                        throw new Error('Server startup timed out');
                    }
                }

                vmIP = vmInfo.externalIP;
                log('VM running with IP: ' + vmIP, 'success');

                // Wait for streaming service to be ready
                updateProgress(70, 'Waiting for streaming service...', 'Checking health...');
                let serviceReady = false;
                let healthPollCount = 0;

                while (!serviceReady && healthPollCount < 60) {
                    healthPollCount++;
                    serviceReady = await checkHealth();

                    if (!serviceReady) {
                        await new Promise(r => setTimeout(r, 2000));
                        updateProgress(Math.min(70 + healthPollCount, 95), 'Waiting for streaming service...', 'Checking health... ' + (healthPollCount * 2) + 's');
                    }
                }

                if (!serviceReady) {
                    throw new Error('Streaming service failed to start');
                }

                updateProgress(100, 'Starting stream...');
                await new Promise(r => setTimeout(r, 500));
                showStream();

            } catch (error) {
                log('Initialization failed: ' + error.message, 'error');
                showError(error.message || 'Failed to connect to server');
            }
        }

        // Handle visibility change
        document.addEventListener('visibilitychange', () => {
            if (!document.hidden && isStreaming) {
                checkHealth();
            }
        });

        // Handle beforeunload
        window.addEventListener('beforeunload', () => {
            navigator.sendBeacon('/api/leaving');
        });

        // Click on status overlay to toggle details
        statusOverlay.addEventListener('click', () => {
            const timeSinceLastSuccess = Math.floor((Date.now() - lastHealthCheckSuccess) / 1000);
            alert('Last successful health check: ' + timeSinceLastSuccess + ' seconds ago');
        });

        initialize();
    </script>
</body>
</html>`;
}

/**
 * Main request handler
 */
export default {
  async fetch(request: Request, env: Env, ctx: ExecutionContext): Promise<Response> {
    const url = new URL(request.url);
    const path = url.pathname;

    try {
      // API: Get VM status (legacy)
      if (path === "/api/vm-status") {
        const status = await getVMStatus(env);
        return new Response(JSON.stringify({ status }), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // API: Get VM info (status + external IP)
      if (path === "/api/vm-info") {
        const info = await getVMInfo(env);
        return new Response(JSON.stringify(info), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // API: Start VM
      if (path === "/api/start-vm" && request.method === "POST") {
        const status = await getVMStatus(env);
        if (status === "RUNNING" || status === "STAGING") {
          return new Response(JSON.stringify({ message: "VM already running or starting" }), {
            headers: { "Content-Type": "application/json" },
          });
        }

        await startVM(env);
        await recordHeartbeat(env); // Record activity
        return new Response(JSON.stringify({ message: "VM start initiated" }), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // API: Stop VM (manual)
      if (path === "/api/stop-vm" && request.method === "POST") {
        await stopVM(env);
        return new Response(JSON.stringify({ message: "VM stop initiated" }), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // API: Client heartbeat - records that a client is actively viewing the page
      if (path === "/api/heartbeat" && request.method === "POST") {
        await recordHeartbeat(env);
        return new Response(JSON.stringify({ ok: true }), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // API: Client leaving (best effort beacon)
      if (path === "/api/leaving" && request.method === "POST") {
        // Don't update heartbeat - let it expire naturally
        return new Response(JSON.stringify({ ok: true }), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // API: Get Sunshine status (direct IP connection)
      if (path === "/api/sunshine-status") {
        const ip = url.searchParams.get("ip");
        if (!ip) {
          // Fall back to getting IP from VM info
          const info = await getVMInfo(env);
          if (!info.externalIP) {
            return new Response(JSON.stringify({ ready: false, active: 0, idle: 0 }), {
              headers: { "Content-Type": "application/json" },
            });
          }
          const status = await checkSunshineStatus(info.externalIP);
          return new Response(JSON.stringify(status), {
            headers: { "Content-Type": "application/json" },
          });
        }
        const status = await checkSunshineStatus(ip);
        return new Response(JSON.stringify(status), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // API: Get idle time (for debugging)
      if (path === "/api/idle-time") {
        const idleSeconds = await getSecondsSinceLastHeartbeat(env);
        return new Response(JSON.stringify({ idle_seconds: idleSeconds }), {
          headers: { "Content-Type": "application/json" },
        });
      }

      // Main page - serve the streaming UI
      if (path === "/" || path === "/index.html") {
        return new Response(generateStreamingPage(HEARTBEAT_INTERVAL_SECONDS * 1000), {
          headers: { "Content-Type": "text/html" },
        });
      }

      return new Response("Not Found", { status: 404 });
    } catch (error) {
      console.error("Error:", error);
      return new Response(`Error: ${error}`, { status: 500 });
    }
  },

  /**
   * Scheduled handler for auto-shutdown
   * DISABLED FOR TESTING - uncomment the logic below to re-enable
   */
  async scheduled(event: ScheduledEvent, env: Env, ctx: ExecutionContext): Promise<void> {
    // Auto-shutdown disabled for testing
    console.log("Auto-shutdown check skipped (disabled for testing)");
    return;

    /*
    try {
      const vmStatus = await getVMStatus(env);

      if (vmStatus !== "RUNNING") {
        console.log("VM not running, skipping shutdown check");
        return;
      }

      const idleSeconds = await getSecondsSinceLastHeartbeat(env);
      console.log(`Idle time: ${idleSeconds}s (threshold: ${IDLE_TIMEOUT_SECONDS}s)`);

      // Auto-shutdown if no heartbeats for 5+ minutes
      if (idleSeconds >= IDLE_TIMEOUT_SECONDS) {
        console.log("No client activity for 5+ minutes, shutting down VM...");
        await stopVM(env);
        console.log("VM shutdown initiated");
      }
    } catch (error) {
      console.error("Auto-shutdown check failed:", error);
    }
    */
  },
};
