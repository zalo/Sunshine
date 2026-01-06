/**
 * @file discord-activity.js
 * @brief Discord Activity client for Sunshine fMP4/MSE streaming
 *
 * Integrates the Discord Embedded App SDK with Sunshine fMP4 streaming,
 * enabling multiplayer game streaming directly within Discord voice channels.
 * Uses WebSocket + fMP4 + MSE instead of WebRTC (which Discord blocks).
 */

class DiscordSunshineActivity {
  constructor() {
    // Debug logging
    this.startTime = Date.now();
    this.debugLogEl = null;

    // Discord SDK instance
    this.discordSdk = null;
    this.auth = null;
    this.currentUser = null;
    this.participants = new Map();

    // MSE streaming state (replaces WebRTC)
    this.streamWs = null;       // WebSocket for fMP4 video stream
    this.inputWs = null;        // WebSocket for input (keyboard/mouse)
    this.mediaSource = null;
    this.sourceBuffer = null;
    this.videoQueue = [];       // Queue for video chunks
    this.isAppending = false;

    // Room state
    this.roomCode = null;
    this.playerId = null;
    this.playerSlot = 1;  // Default to player 1 for input
    this.isHost = true;   // Default to host since there's no WebRTC room system
    this.players = [];

    // Gamepad state
    this.gamepads = new Map();
    this.gamepadPollingId = null;
    this.lastGamepadState = new Map();

    // Input state
    this.keyboardEnabled = true;  // Enable by default for MSE mode
    this.mouseEnabled = true;     // Enable by default for MSE mode
    this.activePointers = new Map();

    // Stats
    this.stats = {
      bitrate: 0,
      fps: 0,
      rtt: 0,
      lastStatsTime: 0,
      lastBytesReceived: 0,
      frameCount: 0
    };
    this.statsIntervalId = null;

    // UI elements
    this.elements = {};

    // Configuration
    this.config = {
      streamUrl: null,
      inputUrl: null,
      gamepadPollRate: 16,
      statsUpdateRate: 1000
    };

    this.init();
  }

  async init() {
    this.cacheElements();
    this.bindEvents();

    this.log('Sunshine Discord Activity v4');
    this.log(`Location: ${window.location.hostname}`);
    this.log(`User Agent: ${navigator.userAgent.substring(0, 60)}...`);

    // Try Discord SDK with timeout, fall back to direct mode
    let discordInitialized = false;
    try {
      discordInitialized = await this.initializeDiscordWithTimeout(5000);
    } catch (error) {
      this.logWarn(`Discord init failed: ${error.message}`);
    }

    if (!discordInitialized) {
      this.logWarn('Running in direct mode (no Discord SDK)');
      this.currentUser = { username: 'Guest', global_name: 'Guest' };
    }

    // Now connect to stream
    try {
      await this.connectToStream();
    } catch (error) {
      this.logError(`Stream connection failed: ${error.message}`);
      this.showError(error.message || 'Failed to connect');
    }
  }

  async initializeDiscordWithTimeout(timeoutMs) {
    this.log(`Waiting for Discord SDK (${timeoutMs/1000}s timeout)...`);

    // Wait for SDK to load or timeout
    const sdkLoaded = await new Promise((resolve) => {
      // Check if already loaded
      if (window.DiscordSDKClass) {
        resolve(true);
        return;
      }

      const timeout = setTimeout(() => {
        this.logWarn('Discord SDK load timeout');
        resolve(false);
      }, timeoutMs);

      window.addEventListener('discord-sdk-loaded', () => {
        clearTimeout(timeout);
        resolve(true);
      }, { once: true });

      window.addEventListener('discord-sdk-error', (e) => {
        clearTimeout(timeout);
        this.logError(`SDK error: ${e.detail}`);
        resolve(false);
      }, { once: true });
    });

    if (!sdkLoaded || !window.DiscordSDKClass) {
      this.log('Discord SDK not available');
      return false;
    }

    this.logSuccess('Discord SDK loaded');

    // Now try full Discord initialization
    try {
      await this.initializeDiscord();
      return true;
    } catch (error) {
      this.logError(`Discord init error: ${error.message}`);
      return false;
    }
  }

  cacheElements() {
    this.elements = {
      loadingScreen: document.getElementById('loadingScreen'),
      loadingStatus: document.getElementById('loadingStatus'),
      debugLog: document.getElementById('debugLog'),
      errorScreen: document.getElementById('errorScreen'),
      errorMessage: document.getElementById('errorMessage'),
      retryBtn: document.getElementById('retryBtn'),
      videoContainer: document.getElementById('videoContainer'),
      videoElement: document.getElementById('videoElement'),
      participantOverlay: document.getElementById('participantOverlay'),
      controlsPanel: document.getElementById('controlsPanel'),
      fullscreenBtn: document.getElementById('fullscreenBtn'),
      keyboardBtn: document.getElementById('keyboardBtn'),
      leaveBtn: document.getElementById('leaveBtn'),
      inviteBtn: document.getElementById('inviteBtn'),
      statsPanel: document.getElementById('statsPanel'),
      statsFps: document.getElementById('statsFps'),
      statsRtt: document.getElementById('statsRtt'),
      statsBitrate: document.getElementById('statsBitrate'),
      gamepadIndicator: document.getElementById('gamepadIndicator'),
      gamepadCount: document.getElementById('gamepadCount'),
      mobileKeyboardInput: document.getElementById('mobileKeyboardInput')
    };
    this.debugLogEl = this.elements.debugLog;
  }

  // Debug logging to screen
  log(message, level = 'info') {
    const elapsed = ((Date.now() - this.startTime) / 1000).toFixed(1);
    const line = document.createElement('div');
    line.className = `debug-line ${level}`;
    line.innerHTML = `<span class="debug-time">[${elapsed}s]</span>${message}`;

    if (this.debugLogEl) {
      this.debugLogEl.appendChild(line);
      this.debugLogEl.scrollTop = this.debugLogEl.scrollHeight;
    }

    // Also log to console
    const consoleFn = level === 'error' ? console.error : level === 'warn' ? console.warn : console.log;
    consoleFn(`[${elapsed}s] ${message}`);
  }

  logSuccess(message) { this.log(message, 'success'); }
  logWarn(message) { this.log(message, 'warn'); }
  logError(message) { this.log(message, 'error'); }

  bindEvents() {
    // Debug toggle
    document.getElementById('debugToggle')?.addEventListener('click', (e) => {
      e.stopPropagation();
      document.getElementById('debugOverlay')?.classList.toggle('minimized');
    });
    document.getElementById('debugHeader')?.addEventListener('click', () => {
      document.getElementById('debugOverlay')?.classList.toggle('minimized');
    });

    // Control buttons
    this.elements.fullscreenBtn?.addEventListener('click', () => this.toggleFullscreen());
    this.elements.keyboardBtn?.addEventListener('click', () => this.toggleMobileKeyboard());
    this.elements.leaveBtn?.addEventListener('click', () => this.leaveActivity());
    this.elements.retryBtn?.addEventListener('click', () => this.retry());
    this.elements.inviteBtn?.addEventListener('click', () => this.openInviteDialog());

    // Mobile keyboard
    if (this.elements.mobileKeyboardInput) {
      this.elements.mobileKeyboardInput.addEventListener('input', (e) => this.handleMobileKeyboardInput(e));
      this.elements.mobileKeyboardInput.addEventListener('keydown', (e) => this.handleMobileKeyDown(e));
    }

    // Pointer events on video
    const videoEl = this.elements.videoElement;
    if (videoEl) {
      videoEl.style.touchAction = 'none';
      videoEl.addEventListener('pointermove', (e) => this.handlePointerMove(e), { passive: false });
      videoEl.addEventListener('pointerdown', (e) => this.handlePointerDown(e), { passive: false });
      videoEl.addEventListener('pointerup', (e) => this.handlePointerUp(e), { passive: false });
      videoEl.addEventListener('pointercancel', (e) => this.handlePointerUp(e), { passive: false });
      videoEl.addEventListener('wheel', (e) => this.handleMouseWheel(e), { passive: false });
      videoEl.addEventListener('contextmenu', (e) => e.preventDefault());
    }

    // Keyboard events
    document.addEventListener('keydown', (e) => this.handleKeyDown(e));
    document.addEventListener('keyup', (e) => this.handleKeyUp(e));

    // Gamepad events
    window.addEventListener('gamepadconnected', (e) => this.handleGamepadConnected(e));
    window.addEventListener('gamepaddisconnected', (e) => this.handleGamepadDisconnected(e));

    // Page visibility
    document.addEventListener('visibilitychange', () => this.handleVisibilityChange());
  }

  // ============== Discord SDK Integration ==============

  async initializeDiscord() {
    this.updateLoadingStatus('Initializing Discord SDK...');
    this.log('Starting Discord SDK initialization...');

    // Check if we're in Discord's iframe
    this.log(`Host: ${window.location.hostname}`);
    this.log(`In Discord iframe: ${window.location.hostname.includes('discordsays.com')}`);

    // Get client ID from environment (injected by server)
    const clientId = window.DISCORD_CLIENT_ID;
    this.log(`Client ID: ${clientId ? clientId.substring(0, 8) + '...' : 'NOT SET'}`);
    if (!clientId) {
      throw new Error('Discord Client ID not configured - check server injection');
    }

    // Wait for Discord SDK to be available (with timeout)
    this.log('Waiting for Discord SDK to load...');
    const sdkTimeout = 10000; // 10 seconds
    const sdkStart = Date.now();

    while (!window.DiscordSDKClass && (Date.now() - sdkStart) < sdkTimeout) {
      await new Promise(r => setTimeout(r, 100));
    }

    const DiscordSDK = window.DiscordSDKClass;
    if (!DiscordSDK) {
      this.logError('Discord SDK failed to load after 10s');
      throw new Error('Discord SDK not loaded - ESM import may have failed');
    }
    this.logSuccess('Discord SDK loaded');

    // Initialize SDK
    this.log('Creating DiscordSDK instance...');
    this.discordSdk = new DiscordSDK(clientId);

    // Wait for ready with timeout
    this.updateLoadingStatus('Waiting for Discord client...');
    this.log('Waiting for Discord client ready signal...');

    const readyPromise = this.discordSdk.ready();
    const timeoutPromise = new Promise((_, reject) =>
      setTimeout(() => reject(new Error('Discord ready timeout (15s) - not running in Discord?')), 15000)
    );

    try {
      await Promise.race([readyPromise, timeoutPromise]);
      this.logSuccess('Discord client ready');
    } catch (error) {
      this.logError(error.message);
      throw error;
    }

    // Authorize with Discord
    this.updateLoadingStatus('Requesting authorization...');
    this.log('Requesting OAuth authorization...');

    try {
      const { code } = await this.discordSdk.commands.authorize({
        client_id: clientId,
        response_type: 'code',
        state: '',
        prompt: 'none',
        scope: ['identify', 'guilds']
      });
      this.logSuccess('Authorization code received');

      // Exchange code for access token via our backend
      this.updateLoadingStatus('Exchanging token...');
      this.log('Exchanging code for access token...');

      const tokenResponse = await fetch('/.proxy/api/token', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ code })
      });

      if (!tokenResponse.ok) {
        const errorText = await tokenResponse.text();
        this.logError(`Token exchange failed: ${tokenResponse.status} - ${errorText}`);
        throw new Error(`Token exchange failed: ${tokenResponse.status}`);
      }

      const { access_token } = await tokenResponse.json();
      this.logSuccess('Access token received');

      // Authenticate with Discord client
      this.updateLoadingStatus('Authenticating...');
      this.log('Authenticating with Discord client...');

      this.auth = await this.discordSdk.commands.authenticate({ access_token });
      this.logSuccess(`Authenticated as: ${this.auth.user.username}`);

      this.currentUser = this.auth.user;

    } catch (error) {
      this.logError(`Auth error: ${error.message}`);
      throw error;
    }

    // Subscribe to participant updates
    this.log('Subscribing to participant updates...');
    try {
      await this.subscribeToParticipants();
      this.logSuccess(`Found ${this.participants.size} participant(s)`);
    } catch (error) {
      this.logWarn(`Participant subscription failed: ${error.message}`);
      // Non-fatal, continue
    }

    // Enable hardware acceleration (required for WebRTC in Discord)
    this.log('Requesting hardware acceleration...');
    try {
      const result = await this.discordSdk.commands.encourageHardwareAcceleration();
      this.logSuccess(`Hardware acceleration: ${result?.enabled ? 'enabled' : 'requested'}`);
    } catch (error) {
      this.logWarn(`Hardware acceleration request: ${error.message}`);
    }

    // Set up signaling URL based on Discord's proxy
    this.config.signalingUrl = '/.proxy/ws/signaling';
    this.log('Discord initialization complete');

    return this.auth;
  }

  async subscribeToParticipants() {
    // Get instance participants (users in the activity)
    const participants = await this.discordSdk.commands.getInstanceConnectedParticipants();

    for (const participant of participants.participants) {
      this.participants.set(participant.id, participant);
    }

    this.updateParticipantOverlay();

    // Subscribe to participant updates
    this.discordSdk.subscribe('ACTIVITY_INSTANCE_PARTICIPANTS_UPDATE', (event) => {
      this.participants.clear();
      for (const participant of event.participants) {
        this.participants.set(participant.id, participant);
      }
      this.updateParticipantOverlay();
    });
  }

  updateParticipantOverlay() {
    if (!this.elements.participantOverlay) return;

    this.elements.participantOverlay.innerHTML = '';

    for (const [id, participant] of this.participants) {
      const avatar = document.createElement('div');
      avatar.className = 'participant-avatar';

      if (id === this.currentUser?.id) {
        avatar.classList.add('host');
      }

      const img = document.createElement('img');
      img.src = participant.avatar
        ? `https://cdn.discordapp.com/avatars/${participant.id}/${participant.avatar}.png?size=64`
        : `https://cdn.discordapp.com/embed/avatars/${parseInt(participant.discriminator || '0') % 5}.png`;
      img.alt = participant.username;

      const tooltip = document.createElement('div');
      tooltip.className = 'participant-tooltip';
      tooltip.textContent = participant.global_name || participant.username;

      avatar.appendChild(img);
      avatar.appendChild(tooltip);
      this.elements.participantOverlay.appendChild(avatar);
    }
  }

  async openInviteDialog() {
    try {
      // openInviteDialog is not available in all contexts
      // It requires the activity to be launched from a guild channel
      if (this.discordSdk.commands.openInviteDialog) {
        await this.discordSdk.commands.openInviteDialog();
      } else {
        // Fallback: try to get shareable URL
        console.log('openInviteDialog not available, activity may not support invites in this context');
        this.showToast('Invite not available in this context');
      }
    } catch (error) {
      console.error('Failed to open invite dialog:', error);
      // Don't show alert, just log and show toast
      this.showToast('Could not open invite dialog');
    }
  }

  // ============== Stream Connection (MSE-based) ==============

  async connectToStream() {
    this.updateLoadingStatus('Connecting to stream...');
    this.log('Starting MSE stream connection...');

    // Determine WebSocket URLs based on context
    const host = window.location.host;
    const hostname = window.location.hostname;
    let baseWsUrl;

    if (hostname.includes('discordsays.com')) {
      // In Discord's iframe - use their proxy
      baseWsUrl = `wss://${host}/.proxy`;
      this.log('Mode: Discord iframe (using /.proxy/)');
    } else if (hostname.includes('modal.run')) {
      // Direct Modal access
      baseWsUrl = `wss://${host}`;
      this.log('Mode: Direct Modal access');
    } else {
      // Local or other
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      baseWsUrl = `${protocol}//${host}`;
      this.log('Mode: Direct/Local access');
    }

    this.config.streamUrl = `${baseWsUrl}/ws/stream`;
    this.config.inputUrl = `${baseWsUrl}/ws/input`;

    try {
      // Check MSE support
      if (!window.MediaSource || !MediaSource.isTypeSupported('video/mp4; codecs="avc1.42E01E"')) {
        throw new Error('MediaSource Extensions (MSE) not supported');
      }
      this.logSuccess('MSE supported');

      // Initialize MSE
      await this.initMediaSource();

      // Connect to input WebSocket
      await this.connectInputWebSocket();

      // Connect to stream WebSocket
      await this.connectStreamWebSocket();

      // Show video container
      this.hideLoading();
      this.showVideoContainer();

      // Start gamepad polling
      this.startGamepadPolling();
      this.startStatsPolling();

      this.showToast('Stream connected');
    } catch (error) {
      this.logError(`Stream connection failed: ${error.message}`);
      throw new Error('Could not connect to stream server: ' + error.message);
    }
  }

  initMediaSource() {
    return new Promise((resolve, reject) => {
      this.log('Initializing MediaSource...');

      this.mediaSource = new MediaSource();
      this.elements.videoElement.src = URL.createObjectURL(this.mediaSource);

      this.mediaSource.addEventListener('sourceopen', () => {
        this.logSuccess('MediaSource opened');

        try {
          // H.264 Baseline Profile, Level 3.1 (matches FFmpeg output)
          const mimeType = 'video/mp4; codecs="avc1.42E01F"';

          if (!MediaSource.isTypeSupported(mimeType)) {
            throw new Error(`Codec not supported: ${mimeType}`);
          }

          this.sourceBuffer = this.mediaSource.addSourceBuffer(mimeType);
          this.sourceBuffer.mode = 'sequence';

          this.sourceBuffer.addEventListener('updateend', () => {
            this.isAppending = false;
            this.processVideoQueue();
          });

          this.sourceBuffer.addEventListener('error', (e) => {
            this.logError(`SourceBuffer error: ${e}`);
          });

          this.logSuccess('SourceBuffer created');
          resolve();
        } catch (error) {
          this.logError(`SourceBuffer creation failed: ${error.message}`);
          reject(error);
        }
      });

      this.mediaSource.addEventListener('sourceended', () => {
        this.log('MediaSource ended');
      });

      this.mediaSource.addEventListener('error', (e) => {
        this.logError(`MediaSource error: ${e}`);
        reject(new Error('MediaSource error'));
      });

      // Timeout
      setTimeout(() => {
        if (this.mediaSource.readyState !== 'open') {
          reject(new Error('MediaSource open timeout'));
        }
      }, 5000);
    });
  }

  connectStreamWebSocket() {
    return new Promise((resolve, reject) => {
      this.log(`Connecting to stream: ${this.config.streamUrl}`);
      this.streamWs = new WebSocket(this.config.streamUrl);
      this.streamWs.binaryType = 'arraybuffer';

      this.streamWs.onopen = () => {
        this.logSuccess('Stream WebSocket connected');
        resolve();
      };

      this.streamWs.onclose = (e) => {
        this.logWarn(`Stream WebSocket closed: code=${e.code}`);
        if (e.code !== 1000) {
          this.showError('Stream disconnected');
        }
      };

      this.streamWs.onerror = () => {
        this.logError('Stream WebSocket error');
        reject(new Error('Stream connection failed'));
      };

      this.streamWs.onmessage = (e) => {
        if (e.data instanceof ArrayBuffer) {
          this.handleVideoData(e.data);
        } else {
          // JSON message (init segment info, etc.)
          try {
            const msg = JSON.parse(e.data);
            this.log(`Stream msg: ${msg.type || 'unknown'}`);
          } catch {}
        }
      };

      setTimeout(() => {
        if (this.streamWs.readyState !== WebSocket.OPEN) {
          reject(new Error('Stream connection timeout'));
        }
      }, 10000);
    });
  }

  connectInputWebSocket() {
    return new Promise((resolve, reject) => {
      this.log(`Connecting to input: ${this.config.inputUrl}`);
      this.inputWs = new WebSocket(this.config.inputUrl);

      this.inputWs.onopen = () => {
        this.logSuccess('Input WebSocket connected');
        resolve();
      };

      this.inputWs.onclose = (e) => {
        this.logWarn(`Input WebSocket closed: code=${e.code}`);
      };

      this.inputWs.onerror = () => {
        this.logError('Input WebSocket error');
        reject(new Error('Input connection failed'));
      };

      this.inputWs.onmessage = (e) => {
        try {
          const msg = JSON.parse(e.data);
          if (msg.type === 'error') {
            this.logError(`Input error: ${msg.message}`);
          }
        } catch {}
      };

      setTimeout(() => {
        if (this.inputWs.readyState !== WebSocket.OPEN) {
          reject(new Error('Input connection timeout'));
        }
      }, 10000);
    });
  }

  handleVideoData(data) {
    // Queue the video data
    this.videoQueue.push(new Uint8Array(data));
    this.stats.lastBytesReceived += data.byteLength;
    this.stats.frameCount++;
    this.processVideoQueue();
  }

  processVideoQueue() {
    if (this.isAppending || this.videoQueue.length === 0) {
      return;
    }

    if (!this.sourceBuffer || this.mediaSource.readyState !== 'open') {
      return;
    }

    // Don't append if there's an ongoing update
    if (this.sourceBuffer.updating) {
      return;
    }

    this.isAppending = true;
    const chunk = this.videoQueue.shift();

    try {
      this.sourceBuffer.appendBuffer(chunk);

      // Try to start playback if video is not playing
      const video = this.elements.videoElement;
      if (video.paused && video.readyState >= 2) {
        video.play().then(() => {
          this.logSuccess('Video playback started!');
        }).catch(err => {
          this.logWarn(`Autoplay blocked: ${err.message}`);
          this.showToast('Click to play');
        });
      }

      // Trim buffer to prevent memory issues (keep last 10 seconds)
      if (!this.sourceBuffer.updating && this.sourceBuffer.buffered.length > 0) {
        const bufferedEnd = this.sourceBuffer.buffered.end(0);
        const bufferedStart = this.sourceBuffer.buffered.start(0);
        if (bufferedEnd - bufferedStart > 10) {
          try {
            this.sourceBuffer.remove(bufferedStart, bufferedEnd - 5);
          } catch {}
        }
      }
    } catch (error) {
      this.isAppending = false;
      this.logError(`Buffer append error: ${error.message}`);

      // If quota exceeded, clear buffer and retry
      if (error.name === 'QuotaExceededError') {
        this.log('Clearing buffer due to quota...');
        try {
          const start = this.sourceBuffer.buffered.start(0);
          const end = this.sourceBuffer.buffered.end(0);
          this.sourceBuffer.remove(start, end - 2);
        } catch {}
      }
    }
  }

  // Send input via WebSocket (JSON format for xdotool)
  sendInput(type, data) {
    if (this.inputWs?.readyState === WebSocket.OPEN) {
      this.inputWs.send(JSON.stringify({ type, ...data }));
    }
  }

  // ============== Input Handling ==============

  handleKeyDown(e) {
    if (!this.keyboardEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    const isVideoFocused = document.activeElement === this.elements.videoElement ||
                           document.activeElement === this.elements.videoContainer;
    if (!isVideoFocused && e.target !== document.body) return;

    e.preventDefault();
    this.sendKeyEvent(e.key, e.code, true);
  }

  handleKeyUp(e) {
    if (!this.keyboardEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    const isVideoFocused = document.activeElement === this.elements.videoElement ||
                           document.activeElement === this.elements.videoContainer;
    if (!isVideoFocused && e.target !== document.body) return;

    e.preventDefault();
    this.sendKeyEvent(e.key, e.code, false);
  }

  sendKeyEvent(key, code, pressed) {
    // Send key via WebSocket to xdotool
    this.sendInput('key', {
      key: key,
      code: code,
      pressed: pressed
    });
  }

  keyCodeToVK(code) {
    const mapping = {
      'KeyA': 0x41, 'KeyB': 0x42, 'KeyC': 0x43, 'KeyD': 0x44,
      'KeyE': 0x45, 'KeyF': 0x46, 'KeyG': 0x47, 'KeyH': 0x48,
      'KeyI': 0x49, 'KeyJ': 0x4A, 'KeyK': 0x4B, 'KeyL': 0x4C,
      'KeyM': 0x4D, 'KeyN': 0x4E, 'KeyO': 0x4F, 'KeyP': 0x50,
      'KeyQ': 0x51, 'KeyR': 0x52, 'KeyS': 0x53, 'KeyT': 0x54,
      'KeyU': 0x55, 'KeyV': 0x56, 'KeyW': 0x57, 'KeyX': 0x58,
      'KeyY': 0x59, 'KeyZ': 0x5A,
      'Digit0': 0x30, 'Digit1': 0x31, 'Digit2': 0x32, 'Digit3': 0x33,
      'Digit4': 0x34, 'Digit5': 0x35, 'Digit6': 0x36, 'Digit7': 0x37,
      'Digit8': 0x38, 'Digit9': 0x39,
      'F1': 0x70, 'F2': 0x71, 'F3': 0x72, 'F4': 0x73,
      'F5': 0x74, 'F6': 0x75, 'F7': 0x76, 'F8': 0x77,
      'F9': 0x78, 'F10': 0x79, 'F11': 0x7A, 'F12': 0x7B,
      'Backspace': 0x08, 'Tab': 0x09, 'Enter': 0x0D, 'Escape': 0x1B, 'Space': 0x20,
      'CapsLock': 0x14, 'NumLock': 0x90, 'ScrollLock': 0x91,
      'PageUp': 0x21, 'PageDown': 0x22, 'End': 0x23, 'Home': 0x24,
      'ArrowLeft': 0x25, 'ArrowUp': 0x26, 'ArrowRight': 0x27, 'ArrowDown': 0x28,
      'Insert': 0x2D, 'Delete': 0x2E,
      'Semicolon': 0xBA, 'Equal': 0xBB, 'Comma': 0xBC, 'Minus': 0xBD,
      'Period': 0xBE, 'Slash': 0xBF, 'Backquote': 0xC0,
      'BracketLeft': 0xDB, 'Backslash': 0xDC, 'BracketRight': 0xDD, 'Quote': 0xDE,
      'ShiftLeft': 0xA0, 'ShiftRight': 0xA1,
      'ControlLeft': 0xA2, 'ControlRight': 0xA3,
      'AltLeft': 0xA4, 'AltRight': 0xA5,
      'MetaLeft': 0x5B, 'MetaRight': 0x5C,
      'PrintScreen': 0x2C, 'Pause': 0x13
    };
    return mapping[code] || 0;
  }

  handlePointerMove(e) {
    e.preventDefault();
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    const videoRect = this.getVideoContentRect();
    if (!videoRect) return;

    const activePointer = this.activePointers.get(e.pointerId);
    if (!activePointer) {
      if (e.clientX < videoRect.left || e.clientX > videoRect.right ||
          e.clientY < videoRect.top || e.clientY > videoRect.bottom) {
        return;
      }
    }

    // Calculate position relative to video content
    const relX = (e.clientX - videoRect.left) / videoRect.width;
    const relY = (e.clientY - videoRect.top) / videoRect.height;

    // Convert to screen coordinates (assuming 1920x1080 display)
    const screenX = Math.round(Math.max(0, Math.min(1, relX)) * 1920);
    const screenY = Math.round(Math.max(0, Math.min(1, relY)) * 1080);

    this.sendMouseMove(screenX, screenY);
  }

  handlePointerDown(e) {
    e.preventDefault();
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    this.elements.videoContainer?.focus();

    if (this.elements.videoElement?.muted) {
      this.elements.videoElement.muted = false;
    }

    if (e.target.setPointerCapture) {
      e.target.setPointerCapture(e.pointerId);
    }

    const button = e.pointerType === 'touch' ? 1 : (e.button + 1); // xdotool uses 1-based buttons
    this.activePointers.set(e.pointerId, { type: e.pointerType, button });

    // Only send mouse events if we can calculate proper coordinates
    const videoRect = this.getVideoContentRect();
    if (!videoRect) return;  // Skip if video dimensions not available

    const relX = (e.clientX - videoRect.left) / videoRect.width;
    const relY = (e.clientY - videoRect.top) / videoRect.height;
    const screenX = Math.round(Math.max(0, Math.min(1, relX)) * 1920);
    const screenY = Math.round(Math.max(0, Math.min(1, relY)) * 1080);
    this.sendMouseMove(screenX, screenY);
    this.sendMouseButton(button, true);
  }

  handlePointerUp(e) {
    e.preventDefault();
    const activePointer = this.activePointers.get(e.pointerId);
    this.activePointers.delete(e.pointerId);

    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    if (e.target.releasePointerCapture) {
      try { e.target.releasePointerCapture(e.pointerId); } catch {}
    }

    // Only send mouse events if we can calculate proper coordinates
    const videoRect = this.getVideoContentRect();
    if (!videoRect) return;  // Skip if video dimensions not available

    const button = activePointer?.button || (e.pointerType === 'touch' ? 1 : (e.button + 1));
    this.sendMouseButton(button, false);
  }

  getVideoContentRect() {
    const video = this.elements.videoElement;
    if (!video || !video.videoWidth || !video.videoHeight) return null;

    const elementRect = video.getBoundingClientRect();
    const videoAspect = video.videoWidth / video.videoHeight;
    const elementAspect = elementRect.width / elementRect.height;

    let contentWidth, contentHeight, contentLeft, contentTop;

    if (videoAspect > elementAspect) {
      contentWidth = elementRect.width;
      contentHeight = elementRect.width / videoAspect;
      contentLeft = elementRect.left;
      contentTop = elementRect.top + (elementRect.height - contentHeight) / 2;
    } else {
      contentHeight = elementRect.height;
      contentWidth = elementRect.height * videoAspect;
      contentTop = elementRect.top;
      contentLeft = elementRect.left + (elementRect.width - contentWidth) / 2;
    }

    return {
      left: contentLeft, top: contentTop,
      right: contentLeft + contentWidth, bottom: contentTop + contentHeight,
      width: contentWidth, height: contentHeight
    };
  }

  handleMouseWheel(e) {
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    e.preventDefault();
    this.sendMouseScroll(e.deltaX, e.deltaY);
  }

  sendMouseMove(x, y) {
    this.sendInput('mousemove', { x, y });
  }

  sendMouseButton(button, pressed) {
    this.sendInput('mousebutton', {
      button: button,  // 1=left, 2=middle, 3=right
      pressed: pressed
    });
  }

  sendMouseScroll(dx, dy) {
    // xdotool uses button 4/5 for scroll
    if (dy < 0) {
      this.sendInput('mousebutton', { button: 5, pressed: true });
      this.sendInput('mousebutton', { button: 5, pressed: false });
    } else if (dy > 0) {
      this.sendInput('mousebutton', { button: 4, pressed: true });
      this.sendInput('mousebutton', { button: 4, pressed: false });
    }
  }

  // ============== Gamepad Handling ==============

  handleGamepadConnected(e) {
    console.log('Gamepad connected:', e.gamepad.index);
    this.updateGamepadIndicator();
    if (this.playerSlot > 0 && !this.gamepads.has(e.gamepad.index)) {
      this.claimGamepad(e.gamepad.index);
    }
  }

  handleGamepadDisconnected(e) {
    const serverSlot = this.gamepads.get(e.gamepad.index);
    if (serverSlot !== undefined) {
      this.releaseGamepad(e.gamepad.index);
    }
    this.updateGamepadIndicator();
  }

  claimGamepad(browserIndex) {
    this.sendSignaling('claim_gamepad', { browser_index: browserIndex });
  }

  releaseGamepad(browserIndex) {
    const serverSlot = this.gamepads.get(browserIndex);
    if (serverSlot !== undefined) {
      this.sendSignaling('release_gamepad', { server_slot: serverSlot });
      this.gamepads.delete(browserIndex);
    }
  }

  handleGamepadClaimed(msg) {
    this.gamepads.set(msg.browser_index, msg.server_slot);
    this.updateGamepadIndicator();
  }

  handleGamepadReleased(msg) {
    for (const [browserIndex, serverSlot] of this.gamepads.entries()) {
      if (serverSlot === msg.server_slot) {
        this.gamepads.delete(browserIndex);
        break;
      }
    }
    this.updateGamepadIndicator();
  }

  startGamepadPolling() {
    if (this.gamepadPollingId) return;
    this.gamepadPollingId = setInterval(() => this.pollGamepads(), this.config.gamepadPollRate);
  }

  stopGamepadPolling() {
    if (this.gamepadPollingId) {
      clearInterval(this.gamepadPollingId);
      this.gamepadPollingId = null;
    }
  }

  pollGamepads() {
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;
    if (this.playerSlot === 0) return;

    const gamepads = navigator.getGamepads();
    for (const gamepad of gamepads) {
      if (!gamepad) continue;

      const serverSlot = this.gamepads.get(gamepad.index);
      if (serverSlot === undefined) {
        if (this.playerSlot > 0) this.claimGamepad(gamepad.index);
        continue;
      }

      const state = this.getGamepadState(gamepad);
      const lastState = this.lastGamepadState.get(gamepad.index);

      if (!lastState || !this.gamepadStatesEqual(state, lastState)) {
        this.sendGamepadState(serverSlot, state);
        this.lastGamepadState.set(gamepad.index, state);
      }
    }
  }

  getGamepadState(gamepad) {
    return {
      buttons: gamepad.buttons.map(b => ({ pressed: b.pressed, value: b.value })),
      axes: Array.from(gamepad.axes)
    };
  }

  gamepadStatesEqual(a, b) {
    if (a.axes.length !== b.axes.length || a.buttons.length !== b.buttons.length) return false;
    for (let i = 0; i < a.axes.length; i++) {
      if (Math.abs(a.axes[i] - b.axes[i]) > 0.01) return false;
    }
    for (let i = 0; i < a.buttons.length; i++) {
      if (a.buttons[i].pressed !== b.buttons[i].pressed) return false;
      if (Math.abs(a.buttons[i].value - b.buttons[i].value) > 0.01) return false;
    }
    return true;
  }

  sendGamepadState(serverSlot, state) {
    // Send gamepad state via WebSocket (for future gamepad support via xdotool/uinput)
    this.sendInput('gamepad', {
      slot: serverSlot,
      buttons: state.buttons.map(b => b.pressed),
      axes: state.axes
    });
  }

  updateGamepadIndicator() {
    const gamepads = navigator.getGamepads();
    const connected = Array.from(gamepads).filter(g => g !== null).length;
    const claimed = this.gamepads.size;

    if (this.elements.gamepadCount) {
      this.elements.gamepadCount.textContent = `${claimed}/${connected}`;
    }
    if (this.elements.gamepadIndicator) {
      this.elements.gamepadIndicator.style.display = connected > 0 ? 'block' : 'none';
      this.elements.gamepadIndicator.classList.toggle('active', claimed > 0);
    }
  }

  // ============== Mobile Keyboard ==============

  toggleMobileKeyboard() {
    const input = this.elements.mobileKeyboardInput;
    const btn = this.elements.keyboardBtn;
    if (!input) return;

    if (document.activeElement === input) {
      input.blur();
      btn?.classList.remove('active');
    } else {
      input.focus();
      btn?.classList.add('active');
    }
  }

  handleMobileKeyboardInput(e) {
    const text = e.target.value;
    if (!text || !this.keyboardEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    // Type the text using xdotool
    this.sendInput('type', { text: text });
    e.target.value = '';
  }

  handleMobileKeyDown(e) {
    if (!this.keyboardEnabled || this.playerSlot === 0) return;
    if (!this.inputWs || this.inputWs.readyState !== WebSocket.OPEN) return;

    const specialKeys = ['Backspace', 'Enter', 'Tab', 'Escape', 'ArrowLeft', 'ArrowUp', 'ArrowRight', 'ArrowDown'];

    if (specialKeys.includes(e.key)) {
      e.preventDefault();
      this.sendKeyEvent(e.key, e.code, true);
      this.sendKeyEvent(e.key, e.code, false);
    }
  }

  charToKeyCode(char) {
    const code = char.toUpperCase().charCodeAt(0);
    if (code >= 65 && code <= 90) return code;
    if (code >= 48 && code <= 57) return code;
    if (char === ' ') return 0x20;

    const punctuation = {
      '.': 0xBE, ',': 0xBC, '/': 0xBF, ';': 0xBA, "'": 0xDE,
      '[': 0xDB, ']': 0xDD, '\\': 0xDC, '-': 0xBD, '=': 0xBB, '`': 0xC0
    };
    return punctuation[char] || null;
  }

  // ============== Stats ==============

  startStatsPolling() {
    this.stats.lastStatsTime = Date.now();
    this.stats.lastBytesReceived = 0;
    this.stats.lastFrameCount = 0;
    this.statsIntervalId = setInterval(() => this.updateStats(), this.config.statsUpdateRate);
  }

  stopStatsPolling() {
    if (this.statsIntervalId) {
      clearInterval(this.statsIntervalId);
      this.statsIntervalId = null;
    }
  }

  updateStats() {
    const now = Date.now();
    const elapsed = (now - this.stats.lastStatsTime) / 1000;

    if (elapsed > 0) {
      // Calculate bitrate from bytes received
      const bytesDelta = this.stats.lastBytesReceived - (this.stats.prevBytesReceived || 0);
      this.stats.bitrate = Math.round((bytesDelta * 8) / elapsed / 1000);

      // Calculate FPS from frame count
      const framesDelta = this.stats.frameCount - (this.stats.lastFrameCount || 0);
      this.stats.fps = Math.round(framesDelta / elapsed);

      // Store previous values
      this.stats.prevBytesReceived = this.stats.lastBytesReceived;
      this.stats.lastFrameCount = this.stats.frameCount;
    }

    this.stats.lastStatsTime = now;

    // Get buffer info for latency estimate
    if (this.sourceBuffer && this.sourceBuffer.buffered.length > 0) {
      const video = this.elements.videoElement;
      const bufferedEnd = this.sourceBuffer.buffered.end(0);
      const currentTime = video.currentTime;
      // Rough latency estimate based on buffer
      this.stats.rtt = Math.round((bufferedEnd - currentTime) * 1000);
    }

    this.updateStatsUI();
  }

  updateStatsUI() {
    if (this.elements.statsFps) {
      this.elements.statsFps.textContent = `${this.stats.fps} fps`;
    }
    if (this.elements.statsRtt) {
      this.elements.statsRtt.textContent = `${this.stats.rtt} ms`;
    }
    if (this.elements.statsBitrate) {
      this.elements.statsBitrate.textContent = `${this.stats.bitrate} kbps`;
    }
  }

  // ============== UI Helpers ==============

  updateLoadingStatus(status) {
    if (this.elements.loadingStatus) {
      this.elements.loadingStatus.textContent = status;
    }
  }

  hideLoading() {
    if (this.elements.loadingScreen) {
      this.elements.loadingScreen.classList.add('hidden');
    }
  }

  showVideoContainer() {
    if (this.elements.videoContainer) {
      this.elements.videoContainer.classList.add('active');
    }
  }

  showError(message) {
    if (this.elements.loadingScreen) {
      this.elements.loadingScreen.classList.add('hidden');
    }
    if (this.elements.errorScreen) {
      this.elements.errorScreen.style.display = 'flex';
    }
    if (this.elements.errorMessage) {
      this.elements.errorMessage.textContent = message;
    }
  }

  showToast(message) {
    const toast = document.createElement('div');
    toast.className = 'toast';
    toast.textContent = message;
    document.body.appendChild(toast);

    setTimeout(() => toast.classList.add('show'), 10);
    setTimeout(() => {
      toast.classList.remove('show');
      setTimeout(() => toast.remove(), 300);
    }, 3000);
  }

  toggleFullscreen() {
    if (document.fullscreenElement) {
      document.exitFullscreen();
    } else {
      this.elements.videoContainer?.requestFullscreen();
    }
  }

  // ============== Lifecycle ==============

  handleVisibilityChange() {
    if (document.hidden) {
      this.stopGamepadPolling();
    } else if (this.playerSlot > 0) {
      this.startGamepadPolling();
    }
  }

  handleSignalingError(msg) {
    this.showError(msg.message || 'Connection error');
  }

  handleDisconnect() {
    this.cleanup();
    this.showError('Connection lost. Please try again.');
  }

  async retry() {
    if (this.elements.errorScreen) {
      this.elements.errorScreen.style.display = 'none';
    }
    if (this.elements.loadingScreen) {
      this.elements.loadingScreen.classList.remove('hidden');
    }

    try {
      await this.connectToStream();
    } catch (error) {
      this.showError(error.message || 'Failed to reconnect');
    }
  }

  async leaveActivity() {
    this.cleanup();
    // Close the activity
    try {
      await this.discordSdk?.commands.close();
    } catch (error) {
      console.error('Failed to close activity:', error);
      // Fallback: just show disconnected state
      this.showError('Disconnected from stream');
    }
  }

  cleanup() {
    this.stopGamepadPolling();
    this.stopStatsPolling();

    // Close WebSocket connections
    if (this.streamWs) {
      this.streamWs.close();
      this.streamWs = null;
    }

    if (this.inputWs) {
      this.inputWs.close();
      this.inputWs = null;
    }

    // Close MediaSource
    if (this.mediaSource && this.mediaSource.readyState === 'open') {
      try {
        this.mediaSource.endOfStream();
      } catch {}
    }
    this.mediaSource = null;
    this.sourceBuffer = null;
    this.videoQueue = [];
    this.isAppending = false;

    this.roomCode = null;
    this.playerId = null;
    this.playerSlot = 0;
    this.isHost = false;
    this.players = [];
    this.gamepads.clear();
    this.lastGamepadState.clear();

    if (this.elements.videoElement) {
      this.elements.videoElement.src = '';
    }
  }
}

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
  window.discordSunshineActivity = new DiscordSunshineActivity();
});
