/**
 * @file play.js
 * @brief WebRTC streaming client for Sunshine multiplayer
 */

class SunshineWebRTC {
  constructor() {
    // Connection state
    this.ws = null;
    this.pc = null;
    this.dataChannel = null;

    // Room state
    this.roomCode = null;
    this.playerId = null;
    this.playerSlot = 0; // 0 = spectator, 1-4 = player
    this.isHost = false;
    this.players = [];

    // Gamepad state
    this.gamepads = new Map(); // browser index -> claimed server slot
    this.gamepadPollingId = null;
    this.lastGamepadState = new Map();

    // Input state
    this.keyboardEnabled = false;
    this.mouseEnabled = false;
    this.pointerLocked = false;

    // Stats
    this.stats = {
      bitrate: 0,
      fps: 0,
      rtt: 0,
      packetsLost: 0,
      framesDecoded: 0,
      lastStatsTime: 0,
      lastBytesReceived: 0,
      lastFramesDecoded: 0
    };
    this.statsIntervalId = null;

    // Pending messages queue (for SDP/ICE that arrive before peer connection is ready)
    this.pendingMessages = [];

    // UI elements
    this.elements = {};

    // Configuration
    // Determine signaling URL based on access method
    let signalingUrl;
    if (window.location.hostname.endsWith('.sels.tech')) {
      // Use sibling subdomain for Cloudflare tunnel (deep subdomains don't get SSL certs)
      signalingUrl = 'wss://sunshine-signaling.sels.tech';
    } else {
      // Local access - WebSocket signaling runs on the main port + 1
      const wsPort = parseInt(window.location.port || '47990') + 1;
      const wsProtocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      signalingUrl = `${wsProtocol}//${window.location.hostname}:${wsPort}`;
    }
    this.config = {
      signalingUrl,
      iceServers: [
        { urls: 'stun:stun.l.google.com:19302' },
        { urls: 'stun:stun1.l.google.com:19302' }
      ],
      gamepadPollRate: 16, // ~60Hz
      statsUpdateRate: 1000
    };

    this.init();
  }

  init() {
    // Cache UI elements
    this.elements = {
      startOverlay: document.getElementById('startOverlay'),
      playerNameInput: document.getElementById('playerName'),
      createRoomBtn: document.getElementById('createRoomBtn'),
      joinCodeInput: document.getElementById('joinCode'),
      joinRoomBtn: document.getElementById('joinRoomBtn'),
      autoJoinBtn: document.getElementById('autoJoinBtn'),
      videoContainer: document.getElementById('videoContainer'),
      videoElement: document.getElementById('videoElement'),
      sidebar: document.getElementById('sidebar'),
      sidebarToggle: document.getElementById('sidebarToggle'),
      roomCodeDisplay: document.getElementById('roomCodeDisplay'),
      copyCodeBtn: document.getElementById('copyCodeBtn'),
      playerList: document.getElementById('playerList'),
      joinPlayerSection: document.getElementById('joinPlayerSection'),
      joinAsPlayerBtn: document.getElementById('joinAsPlayerBtn'),
      permissionsPanel: document.getElementById('permissionsPanel'),
      qualityPanel: document.getElementById('qualityPanel'),
      bitrateSlider: document.getElementById('bitrateSlider'),
      bitrateValue: document.getElementById('bitrateValue'),
      framerateSelect: document.getElementById('framerateSelect'),
      resolutionSelect: document.getElementById('resolutionSelect'),
      applyQualityBtn: document.getElementById('applyQualityBtn'),
      statsBitrate: document.getElementById('statsBitrate'),
      statsFps: document.getElementById('statsFps'),
      statsRtt: document.getElementById('statsRtt'),
      statsPacketLoss: document.getElementById('statsPacketLoss'),
      gamepadIndicator: document.getElementById('gamepadIndicator'),
      fullscreenBtn: document.getElementById('fullscreenBtn'),
      leaveBtn: document.getElementById('leaveBtn'),
      connectionStatus: document.getElementById('connectionStatus'),
      allowKeyboard: document.getElementById('allowKeyboard'),
      allowMouse: document.getElementById('allowMouse')
    };

    this.bindEvents();
    this.loadSavedName();
  }

  bindEvents() {
    // Start overlay events - support both old room-based and new auto-join
    this.elements.createRoomBtn?.addEventListener('click', () => this.autoJoin());
    this.elements.joinRoomBtn?.addEventListener('click', () => this.autoJoin());
    this.elements.autoJoinBtn?.addEventListener('click', () => this.autoJoin());
    this.elements.joinCodeInput?.addEventListener('keypress', (e) => {
      if (e.key === 'Enter') this.autoJoin();
    });
    this.elements.playerNameInput?.addEventListener('keypress', (e) => {
      if (e.key === 'Enter') this.autoJoin();
    });

    // Format room code input (uppercase, max 6 chars) - kept for backwards compatibility
    this.elements.joinCodeInput?.addEventListener('input', (e) => {
      e.target.value = e.target.value.toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 6);
    });

    // Sidebar events
    this.elements.sidebarToggle?.addEventListener('click', () => this.toggleSidebar());
    this.elements.copyCodeBtn?.addEventListener('click', () => this.copyRoomCode());
    this.elements.joinAsPlayerBtn?.addEventListener('click', () => this.requestJoinAsPlayer());
    this.elements.fullscreenBtn?.addEventListener('click', () => this.toggleFullscreen());
    this.elements.leaveBtn?.addEventListener('click', () => this.leaveRoom());

    // Permission toggle events (host only)
    this.elements.allowKeyboard?.addEventListener('change', (e) => {
      this.setGuestKeyboardPermission(e.target.checked);
    });
    this.elements.allowMouse?.addEventListener('change', (e) => {
      this.setGuestMousePermission(e.target.checked);
    });

    // Quality settings events
    this.elements.bitrateSlider?.addEventListener('input', (e) => {
      if (this.elements.bitrateValue) {
        this.elements.bitrateValue.textContent = e.target.value;
      }
    });
    this.elements.applyQualityBtn?.addEventListener('click', () => this.applyQualitySettings());

    // Video events - click to focus for keyboard input and unmute audio
    this.elements.videoElement?.addEventListener('click', () => {
      // Focus the video container to enable keyboard input
      this.elements.videoContainer?.focus();
      // Unmute audio on user interaction (required by browser autoplay policies)
      if (this.elements.videoElement.muted) {
        this.elements.videoElement.muted = false;
        console.log('Audio unmuted on user interaction');
      }
    });
    // Make video container focusable
    if (this.elements.videoContainer) {
      this.elements.videoContainer.tabIndex = 0;
    }

    // Keyboard events
    document.addEventListener('keydown', (e) => this.handleKeyDown(e));
    document.addEventListener('keyup', (e) => this.handleKeyUp(e));

    // Mouse events
    document.addEventListener('mousemove', (e) => this.handleMouseMove(e));
    document.addEventListener('mousedown', (e) => this.handleMouseButton(e, true));
    document.addEventListener('mouseup', (e) => this.handleMouseButton(e, false));
    document.addEventListener('wheel', (e) => this.handleMouseWheel(e), { passive: false });

    // Pointer lock events
    document.addEventListener('pointerlockchange', () => this.handlePointerLockChange());
    document.addEventListener('pointerlockerror', () => this.handlePointerLockError());

    // Gamepad events
    window.addEventListener('gamepadconnected', (e) => this.handleGamepadConnected(e));
    window.addEventListener('gamepaddisconnected', (e) => this.handleGamepadDisconnected(e));

    // Page visibility
    document.addEventListener('visibilitychange', () => this.handleVisibilityChange());

    // Before unload
    window.addEventListener('beforeunload', () => this.cleanup());
  }

  loadSavedName() {
    const savedName = localStorage.getItem('sunshinePlayerName');
    if (savedName && this.elements.playerNameInput) {
      this.elements.playerNameInput.value = savedName;
    }
  }

  savePlayerName(name) {
    localStorage.setItem('sunshinePlayerName', name);
  }

  // ============== Signaling ==============

  connectSignaling() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(this.config.signalingUrl);

      this.ws.onopen = () => {
        console.log('Signaling connected');
        this.updateConnectionStatus('connected');
        resolve();
      };

      this.ws.onclose = (e) => {
        console.log('Signaling disconnected:', e.code, e.reason);
        this.updateConnectionStatus('disconnected');
        this.handleDisconnect();
      };

      this.ws.onerror = (e) => {
        console.error('Signaling error:', e);
        this.updateConnectionStatus('error');
        reject(e);
      };

      this.ws.onmessage = (e) => {
        try {
          const msg = JSON.parse(e.data);
          this.handleSignalingMessage(msg);
        } catch (err) {
          console.error('Failed to parse signaling message:', err);
        }
      };
    });
  }

  sendSignaling(type, payload = {}) {
    if (this.ws?.readyState === WebSocket.OPEN) {
      this.ws.send(JSON.stringify({ type, ...payload }));
    }
  }

  handleSignalingMessage(msg) {
    console.log('Signaling message:', msg.type, msg);

    switch (msg.type) {
      case 'room_created':
        this.handleRoomCreated(msg);
        break;
      case 'room_joined':
        this.handleRoomJoined(msg);
        break;
      case 'room_updated':
        this.handleRoomUpdated(msg);
        break;
      case 'promoted_to_player':
        this.handlePromotedToPlayer(msg);
        break;
      case 'gamepad_claimed':
        this.handleGamepadClaimed(msg);
        break;
      case 'gamepad_released':
        this.handleGamepadReleased(msg);
        break;
      case 'stream_ready':
        this.handleStreamReady(msg);
        break;
      case 'sdp':
        this.handleRemoteSDP(msg);
        break;
      case 'ice':
        this.handleRemoteICE(msg);
        break;
      case 'error':
        this.handleSignalingError(msg);
        break;
      case 'peer_joined':
        this.showNotification(`${msg.name} joined`);
        break;
      case 'peer_left':
        this.showNotification(`${msg.name} left`);
        break;
      case 'quality_updated':
        this.handleQualityUpdated(msg);
        break;
      default:
        console.warn('Unknown signaling message type:', msg.type);
    }
  }

  // ============== Room Management ==============

  async createRoom() {
    // Legacy - redirect to autoJoin
    this.autoJoin();
  }

  async joinRoom() {
    // Legacy - redirect to autoJoin
    this.autoJoin();
  }

  /**
   * Simplified join - automatically joins the single stream session
   * First user becomes host, subsequent users become guests
   */
  async autoJoin() {
    const name = this.elements.playerNameInput?.value.trim() || 'Player';
    this.savePlayerName(name);

    try {
      this.showLoading('Connecting...');
      await this.connectSignaling();
      // Use new simplified 'join' message - server determines if host or guest
      this.sendSignaling('join', { player_name: name });
    } catch (err) {
      this.hideLoading();
      this.showError('Failed to connect to server');
    }
  }

  handleRoomCreated(msg) {
    this.hideLoading();
    this.roomCode = msg.room_code;
    this.playerId = msg.peer_id;
    this.playerSlot = 1; // Host is always Player 1
    this.isHost = true;
    this.players = msg.players || [];

    // Set initial permissions from server
    this.keyboardEnabled = msg.keyboard_enabled ?? true;  // Default true for host
    this.mouseEnabled = msg.mouse_enabled ?? true;        // Default true for host

    console.log('Room created - keyboard:', this.keyboardEnabled, 'mouse:', this.mouseEnabled);

    // Initialize peer connection immediately - server sends SDP right after room creation
    this.initPeerConnection();

    this.showStreamUI();
    this.updateRoomUI();
  }

  handleRoomJoined(msg) {
    this.hideLoading();
    this.roomCode = msg.room_code;
    this.playerId = msg.peer_id;
    this.playerSlot = msg.slot || msg.player_slot || 0;
    this.isHost = msg.is_host || false;
    this.players = msg.players || [];

    // Set initial permissions from server
    this.keyboardEnabled = msg.keyboard_enabled ?? false;
    this.mouseEnabled = msg.mouse_enabled ?? false;

    console.log('Room joined - keyboard:', this.keyboardEnabled, 'mouse:', this.mouseEnabled, 'slot:', this.playerSlot);

    // Initialize peer connection immediately - server sends SDP right after room join
    this.initPeerConnection();

    this.showStreamUI();
    this.updateRoomUI();
  }

  handleRoomUpdated(msg) {
    this.players = msg.players || [];
    this.updatePlayerList();
  }

  requestJoinAsPlayer() {
    if (this.playerSlot === 0) {
      this.sendSignaling('join_as_player');
    }
  }

  handlePromotedToPlayer(msg) {
    this.playerSlot = msg.slot;
    this.showNotification(`You are now Player ${msg.slot}`);
    this.updateRoomUI();

    // Enable gamepad polling when becoming a player
    this.startGamepadPolling();
  }

  leaveRoom() {
    this.sendSignaling('leave_room');
    this.cleanup();
    this.showStartOverlay();
  }

  // ============== WebRTC Connection ==============

  handleStreamReady(msg) {
    // Server is ready to send stream
    // Note: Peer connection is already initialized in handleRoomCreated/handleRoomJoined
    // Only update ICE servers if provided and we don't have a connection yet
    if (msg.ice_servers && !this.pc) {
      this.config.iceServers = msg.ice_servers;
      this.initPeerConnection();
    }
    // Otherwise, the stream is ready on the existing connection
    console.log('Stream ready on existing connection');
  }

  initPeerConnection() {
    const config = {
      iceServers: this.config.iceServers,
      iceCandidatePoolSize: 10
    };

    this.pc = new RTCPeerConnection(config);

    this.pc.onicecandidate = (e) => {
      if (e.candidate) {
        this.sendSignaling('ice', {
          candidate: e.candidate.candidate,
          sdpMid: e.candidate.sdpMid,
          sdpMLineIndex: e.candidate.sdpMLineIndex
        });
      }
    };

    this.pc.oniceconnectionstatechange = () => {
      console.log('ICE connection state:', this.pc.iceConnectionState);
      this.updateConnectionStatus(this.pc.iceConnectionState);
    };

    this.pc.onconnectionstatechange = () => {
      console.log('Connection state:', this.pc.connectionState);
    };

    this.pc.ontrack = (e) => {
      console.log('Track received:', e.track.kind);
      if (e.track.kind === 'video' || e.track.kind === 'audio') {
        // Add both video and audio tracks to the same media stream
        if (!this.elements.videoElement.srcObject) {
          this.elements.videoElement.srcObject = e.streams[0];
        }
        // Explicitly play to handle autoplay policies
        this.elements.videoElement.play().catch(err => {
          console.log('Media autoplay blocked, will play on user interaction:', err);
        });
      }
    };

    this.pc.ondatachannel = (e) => {
      console.log('Data channel received:', e.channel.label);
      if (e.channel.label === 'input') {
        this.setupDataChannel(e.channel);
      }
    };

    // Note: Server creates the offer, so we don't create one here.
    // Data channel will be received via ondatachannel event.

    // Process any queued messages that arrived before peer connection was ready
    this.processPendingMessages();
  }

  async processPendingMessages() {
    console.log(`Processing ${this.pendingMessages.length} pending messages`);
    for (const pending of this.pendingMessages) {
      if (pending.type === 'sdp') {
        await this.handleRemoteSDP(pending.msg);
      } else if (pending.type === 'ice') {
        await this.handleRemoteICE(pending.msg);
      }
    }
    this.pendingMessages = [];
  }

  setupDataChannel(channel) {
    channel.onopen = () => {
      console.log('Data channel open');
      this.startStatsPolling();
    };

    channel.onclose = () => {
      console.log('Data channel closed');
    };

    channel.onerror = (e) => {
      console.error('Data channel error:', e);
    };

    channel.onmessage = (e) => {
      // Handle any server->client messages on data channel
      try {
        const msg = JSON.parse(e.data);
        this.handleDataChannelMessage(msg);
      } catch (err) {
        // Binary data or non-JSON
      }
    };

    this.dataChannel = channel;
  }

  handleDataChannelMessage(msg) {
    // Server can send permission updates, etc.
    switch (msg.type) {
      case 'permissions_update':
        this.keyboardEnabled = msg.keyboard;
        this.mouseEnabled = msg.mouse;
        this.updatePermissionsUI();
        break;
    }
  }

  async createOffer() {
    try {
      // Add transceivers for receiving media
      this.pc.addTransceiver('video', { direction: 'recvonly' });
      this.pc.addTransceiver('audio', { direction: 'recvonly' });

      const offer = await this.pc.createOffer();
      await this.pc.setLocalDescription(offer);

      this.sendSignaling('sdp', {
        type: 'offer',
        sdp: offer.sdp
      });
    } catch (err) {
      console.error('Failed to create offer:', err);
    }
  }

  async handleRemoteSDP(msg) {
    try {
      if (!this.pc) {
        console.log('Queueing SDP (peer connection not ready yet)');
        this.pendingMessages.push({ type: 'sdp', msg });
        return;
      }

      const desc = new RTCSessionDescription({
        type: msg.sdp_type || 'answer',
        sdp: msg.sdp
      });
      await this.pc.setRemoteDescription(desc);

      if (desc.type === 'offer') {
        const answer = await this.pc.createAnswer();
        await this.pc.setLocalDescription(answer);
        this.sendSignaling('sdp', {
          sdp_type: 'answer',
          sdp: answer.sdp
        });
      }
    } catch (err) {
      console.error('Failed to handle remote SDP:', err);
    }
  }

  async handleRemoteICE(msg) {
    try {
      if (!this.pc) {
        // Queue ICE candidates that arrive before peer connection is ready
        this.pendingMessages.push({ type: 'ice', msg });
        return;
      }

      // Server sends 'mid', RTCIceCandidate expects 'sdpMid'
      const candidate = new RTCIceCandidate({
        candidate: msg.candidate,
        sdpMid: msg.sdpMid || msg.mid,
        sdpMLineIndex: msg.sdpMLineIndex
      });
      await this.pc.addIceCandidate(candidate);
    } catch (err) {
      console.error('Failed to add ICE candidate:', err);
    }
  }

  // ============== Gamepad Input ==============

  handleGamepadConnected(e) {
    console.log('Gamepad connected:', e.gamepad.index, e.gamepad.id);
    this.updateGamepadIndicator();

    // Auto-claim if we're a player
    if (this.playerSlot > 0 && !this.gamepads.has(e.gamepad.index)) {
      this.claimGamepad(e.gamepad.index);
    }
  }

  handleGamepadDisconnected(e) {
    console.log('Gamepad disconnected:', e.gamepad.index);

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
    console.log(`Gamepad ${msg.browser_index} claimed as server slot ${msg.server_slot}`);
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

    this.gamepadPollingId = setInterval(() => {
      this.pollGamepads();
    }, this.config.gamepadPollRate);
  }

  stopGamepadPolling() {
    if (this.gamepadPollingId) {
      clearInterval(this.gamepadPollingId);
      this.gamepadPollingId = null;
    }
  }

  pollGamepads() {
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;
    if (this.playerSlot === 0) return; // Spectators don't send input

    const gamepads = navigator.getGamepads();

    for (const gamepad of gamepads) {
      if (!gamepad) continue;

      const serverSlot = this.gamepads.get(gamepad.index);
      if (serverSlot === undefined) {
        // Auto-claim unassigned gamepads
        if (this.playerSlot > 0) {
          this.claimGamepad(gamepad.index);
        }
        continue;
      }

      const state = this.getGamepadState(gamepad);
      const lastState = this.lastGamepadState.get(gamepad.index);

      // Only send if state changed
      if (!lastState || !this.gamepadStatesEqual(state, lastState)) {
        this.sendGamepadState(serverSlot, state);
        this.lastGamepadState.set(gamepad.index, state);
      }
    }
  }

  getGamepadState(gamepad) {
    return {
      buttons: gamepad.buttons.map(b => ({
        pressed: b.pressed,
        value: b.value
      })),
      axes: Array.from(gamepad.axes)
    };
  }

  gamepadStatesEqual(a, b) {
    if (a.axes.length !== b.axes.length) return false;
    if (a.buttons.length !== b.buttons.length) return false;

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
    // Pack gamepad state into binary format
    // Format: [type:1][slot:1][buttons:2][axes:8*2]
    const buffer = new ArrayBuffer(20);
    const view = new DataView(buffer);

    view.setUint8(0, 0x01); // Type: gamepad
    view.setUint8(1, serverSlot);

    // Pack button states into a bitmask (up to 16 buttons)
    let buttonMask = 0;
    for (let i = 0; i < Math.min(16, state.buttons.length); i++) {
      if (state.buttons[i].pressed) {
        buttonMask |= (1 << i);
      }
    }
    view.setUint16(2, buttonMask, true);

    // Pack axes as int16 (-32768 to 32767)
    for (let i = 0; i < Math.min(4, state.axes.length); i++) {
      const axisValue = Math.round(state.axes[i] * 32767);
      view.setInt16(4 + i * 2, axisValue, true);
    }

    // Trigger values (buttons 6 and 7 typically)
    const lt = state.buttons[6]?.value || 0;
    const rt = state.buttons[7]?.value || 0;
    view.setUint8(12, Math.round(lt * 255));
    view.setUint8(13, Math.round(rt * 255));

    this.dataChannel.send(buffer);
  }

  // ============== Keyboard Input ==============

  handleKeyDown(e) {
    if (!this.keyboardEnabled || this.playerSlot === 0) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    // Only capture keyboard when video container is focused or mouse is over video
    const activeElement = document.activeElement;
    const isVideoFocused = activeElement === this.elements.videoElement ||
                           activeElement === this.elements.videoContainer ||
                           this.elements.videoContainer?.contains(activeElement);
    if (!isVideoFocused) return;

    e.preventDefault();
    this.sendKeyEvent(e.code, true, e.repeat);
  }

  handleKeyUp(e) {
    if (!this.keyboardEnabled || this.playerSlot === 0) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    // Only capture keyboard when video container is focused
    const activeElement = document.activeElement;
    const isVideoFocused = activeElement === this.elements.videoElement ||
                           activeElement === this.elements.videoContainer ||
                           this.elements.videoContainer?.contains(activeElement);
    if (!isVideoFocused) return;

    e.preventDefault();
    this.sendKeyEvent(e.code, false, false);
  }

  sendKeyEvent(code, pressed, repeat) {
    // Format: type(1) | key_code(2) | modifiers(1) | pressed(1) = 5 bytes
    const buffer = new ArrayBuffer(5);
    const view = new DataView(buffer);

    view.setUint8(0, 0x02);  // Type: keyboard
    view.setUint16(1, this.keyCodeToVK(code), true);  // key_code (Windows VK code, little-endian)
    view.setUint8(3, 0);     // modifiers (none for now)
    view.setUint8(4, pressed ? 1 : 0);  // pressed

    this.dataChannel.send(buffer);
  }

  keyCodeToVK(code) {
    // Map JavaScript key codes to Windows Virtual Key codes (VK_*)
    const mapping = {
      // Letters (VK_A-VK_Z = 0x41-0x5A)
      'KeyA': 0x41, 'KeyB': 0x42, 'KeyC': 0x43, 'KeyD': 0x44,
      'KeyE': 0x45, 'KeyF': 0x46, 'KeyG': 0x47, 'KeyH': 0x48,
      'KeyI': 0x49, 'KeyJ': 0x4A, 'KeyK': 0x4B, 'KeyL': 0x4C,
      'KeyM': 0x4D, 'KeyN': 0x4E, 'KeyO': 0x4F, 'KeyP': 0x50,
      'KeyQ': 0x51, 'KeyR': 0x52, 'KeyS': 0x53, 'KeyT': 0x54,
      'KeyU': 0x55, 'KeyV': 0x56, 'KeyW': 0x57, 'KeyX': 0x58,
      'KeyY': 0x59, 'KeyZ': 0x5A,
      // Numbers (VK_0-VK_9 = 0x30-0x39)
      'Digit0': 0x30, 'Digit1': 0x31, 'Digit2': 0x32, 'Digit3': 0x33,
      'Digit4': 0x34, 'Digit5': 0x35, 'Digit6': 0x36, 'Digit7': 0x37,
      'Digit8': 0x38, 'Digit9': 0x39,
      // Function keys (VK_F1-VK_F12 = 0x70-0x7B)
      'F1': 0x70, 'F2': 0x71, 'F3': 0x72, 'F4': 0x73,
      'F5': 0x74, 'F6': 0x75, 'F7': 0x76, 'F8': 0x77,
      'F9': 0x78, 'F10': 0x79, 'F11': 0x7A, 'F12': 0x7B,
      // Control keys
      'Backspace': 0x08, 'Tab': 0x09, 'Enter': 0x0D, 'Escape': 0x1B, 'Space': 0x20,
      'CapsLock': 0x14, 'NumLock': 0x90, 'ScrollLock': 0x91,
      // Navigation keys
      'PageUp': 0x21, 'PageDown': 0x22, 'End': 0x23, 'Home': 0x24,
      'ArrowLeft': 0x25, 'ArrowUp': 0x26, 'ArrowRight': 0x27, 'ArrowDown': 0x28,
      'Insert': 0x2D, 'Delete': 0x2E,
      // Punctuation (OEM keys)
      'Semicolon': 0xBA, 'Equal': 0xBB, 'Comma': 0xBC, 'Minus': 0xBD,
      'Period': 0xBE, 'Slash': 0xBF, 'Backquote': 0xC0,
      'BracketLeft': 0xDB, 'Backslash': 0xDC, 'BracketRight': 0xDD, 'Quote': 0xDE,
      // Modifier keys
      'ShiftLeft': 0xA0, 'ShiftRight': 0xA1,
      'ControlLeft': 0xA2, 'ControlRight': 0xA3,
      'AltLeft': 0xA4, 'AltRight': 0xA5,
      'MetaLeft': 0x5B, 'MetaRight': 0x5C,
      // Special
      'PrintScreen': 0x2C, 'Pause': 0x13
    };

    return mapping[code] || 0;
  }

  // ============== Mouse Input ==============

  handleMouseMove(e) {
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    // Only handle mouse over the video element
    if (!this.elements.videoElement) return;
    const rect = this.elements.videoElement.getBoundingClientRect();

    // Check if mouse is over video
    if (e.clientX < rect.left || e.clientX > rect.right ||
        e.clientY < rect.top || e.clientY > rect.bottom) {
      return;
    }

    // Calculate normalized absolute position (0-65535)
    const relX = (e.clientX - rect.left) / rect.width;
    const relY = (e.clientY - rect.top) / rect.height;
    const absX = Math.round(relX * 65535);
    const absY = Math.round(relY * 65535);

    this.sendMouseMoveAbs(absX, absY);
  }

  handleMouseButton(e, pressed) {
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    // Only handle clicks on the video element
    if (e.target !== this.elements.videoElement) return;

    e.preventDefault();
    this.sendMouseButton(e.button, pressed);
  }

  handleMouseWheel(e) {
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    // Only handle scroll over the video element
    if (e.target !== this.elements.videoElement) return;

    e.preventDefault();
    // Negate deltaY to fix scroll direction (browser deltaY is inverted)
    this.sendMouseScroll(e.deltaX, -e.deltaY);
  }

  sendMouseMoveAbs(absX, absY) {
    // Format: type(1) | flags(1) | abs_x(2) | abs_y(2) = 6 bytes
    // flags: 0x01 = absolute mode
    const buffer = new ArrayBuffer(6);
    const view = new DataView(buffer);

    view.setUint8(0, 0x03); // Type: mouse move
    view.setUint8(1, 0x01); // Flags: absolute mode
    view.setUint16(2, absX, true);
    view.setUint16(4, absY, true);

    this.dataChannel.send(buffer);
  }

  sendMouseButton(button, pressed) {
    const buffer = new ArrayBuffer(3);
    const view = new DataView(buffer);

    view.setUint8(0, 0x04); // Type: mouse button
    view.setUint8(1, button);
    view.setUint8(2, pressed ? 1 : 0);

    this.dataChannel.send(buffer);
  }

  sendMouseScroll(dx, dy) {
    const buffer = new ArrayBuffer(6);
    const view = new DataView(buffer);

    view.setUint8(0, 0x05); // Type: mouse scroll
    view.setUint8(1, 0); // Reserved
    view.setInt16(2, Math.round(dx), true);
    view.setInt16(4, Math.round(dy), true);

    this.dataChannel.send(buffer);
  }

  // ============== Pointer Lock ==============

  requestPointerLock() {
    if (this.playerSlot === 0) return;
    if (!this.keyboardEnabled && !this.mouseEnabled) return;

    this.elements.videoElement?.requestPointerLock();
  }

  handlePointerLockChange() {
    this.pointerLocked = document.pointerLockElement === this.elements.videoElement;
    console.log('Pointer lock:', this.pointerLocked);
  }

  handlePointerLockError() {
    console.error('Pointer lock error');
    this.pointerLocked = false;
  }

  // ============== Stats ==============

  startStatsPolling() {
    this.statsIntervalId = setInterval(() => {
      this.updateStats();
    }, this.config.statsUpdateRate);
  }

  stopStatsPolling() {
    if (this.statsIntervalId) {
      clearInterval(this.statsIntervalId);
      this.statsIntervalId = null;
    }
  }

  async updateStats() {
    if (!this.pc) return;

    try {
      const stats = await this.pc.getStats();
      const now = Date.now();

      stats.forEach(report => {
        if (report.type === 'inbound-rtp' && report.kind === 'video') {
          const bytesReceived = report.bytesReceived || 0;
          const framesDecoded = report.framesDecoded || 0;

          if (this.stats.lastStatsTime > 0) {
            const elapsed = (now - this.stats.lastStatsTime) / 1000;
            const bytesDelta = bytesReceived - this.stats.lastBytesReceived;
            const framesDelta = framesDecoded - this.stats.lastFramesDecoded;

            this.stats.bitrate = Math.round((bytesDelta * 8) / elapsed / 1000); // kbps
            this.stats.fps = Math.round(framesDelta / elapsed);
          }

          this.stats.lastBytesReceived = bytesReceived;
          this.stats.lastFramesDecoded = framesDecoded;
          this.stats.packetsLost = report.packetsLost || 0;
        }

        if (report.type === 'candidate-pair' && report.state === 'succeeded') {
          this.stats.rtt = report.currentRoundTripTime
            ? Math.round(report.currentRoundTripTime * 1000)
            : 0;
        }
      });

      this.stats.lastStatsTime = now;
      this.updateStatsUI();
    } catch (err) {
      console.error('Failed to get stats:', err);
    }
  }

  // ============== UI Updates ==============

  showStartOverlay() {
    if (this.elements.startOverlay) {
      this.elements.startOverlay.style.display = 'flex';
    }
    if (this.elements.videoContainer) {
      this.elements.videoContainer.classList.add('hidden');
    }
    if (this.elements.sidebar) {
      this.elements.sidebar.classList.add('hidden');
    }
  }

  showStreamUI() {
    if (this.elements.startOverlay) {
      this.elements.startOverlay.style.display = 'none';
    }
    if (this.elements.videoContainer) {
      this.elements.videoContainer.classList.remove('hidden');
    }
    if (this.elements.sidebar) {
      this.elements.sidebar.classList.remove('hidden');
      this.elements.sidebar.classList.add('open'); // Show sidebar by default
    }
    if (this.elements.sidebarToggle) {
      this.elements.sidebarToggle.classList.remove('hidden');
    }
    if (this.elements.fullscreenBtn) {
      this.elements.fullscreenBtn.classList.remove('hidden');
    }
  }

  updateRoomUI() {
    // Update room code display
    if (this.elements.roomCodeDisplay) {
      this.elements.roomCodeDisplay.textContent = this.roomCode || '';
    }

    // Update player list
    this.updatePlayerList();

    // Show/hide spectator section (only show if spectator)
    if (this.elements.joinPlayerSection) {
      this.elements.joinPlayerSection.style.display = this.playerSlot === 0 ? 'block' : 'none';
    }

    // Show/hide permissions panel (host only)
    if (this.elements.permissionsPanel) {
      this.elements.permissionsPanel.style.display = this.isHost ? 'block' : 'none';
    }

    // Show/hide quality panel (host only)
    if (this.elements.qualityPanel) {
      this.elements.qualityPanel.style.display = this.isHost ? 'block' : 'none';
    }

    // Start gamepad polling if we're a player
    if (this.playerSlot > 0) {
      this.startGamepadPolling();
    }
  }

  updatePlayerList() {
    if (!this.elements.playerList) return;

    this.elements.playerList.innerHTML = '';

    for (const player of this.players) {
      const item = document.createElement('div');
      item.className = 'player-item';

      const slot = player.slot || 0;
      const isMe = player.peer_id === this.playerId;
      const statusClass = slot > 0 ? 'player' : 'spectator';

      item.innerHTML = `
        <span class="player-slot ${statusClass}">
          ${slot > 0 ? `P${slot}` : 'S'}
        </span>
        <span class="player-name">${player.name}${isMe ? ' (You)' : ''}</span>
        <span class="player-gamepads">${player.gamepad_count || 0} GP</span>
      `;

      this.elements.playerList.appendChild(item);
    }
  }

  updateStatsUI() {
    if (this.elements.statsBitrate) {
      this.elements.statsBitrate.textContent = `${this.stats.bitrate} kbps`;
    }
    if (this.elements.statsFps) {
      this.elements.statsFps.textContent = `${this.stats.fps} fps`;
    }
    if (this.elements.statsRtt) {
      this.elements.statsRtt.textContent = `${this.stats.rtt} ms`;
    }
    if (this.elements.statsPacketLoss) {
      this.elements.statsPacketLoss.textContent = `${this.stats.packetsLost}`;
    }
  }

  updateGamepadIndicator() {
    if (!this.elements.gamepadIndicator) return;

    const gamepads = navigator.getGamepads();
    const connected = Array.from(gamepads).filter(g => g !== null).length;
    const claimed = this.gamepads.size;

    const countEl = document.getElementById('gamepadCount');
    if (countEl) {
      countEl.textContent = `${claimed}/${connected}`;
    }
    this.elements.gamepadIndicator.style.display = connected > 0 ? 'flex' : 'none';
    this.elements.gamepadIndicator.classList.toggle('active', claimed > 0);
  }

  updatePermissionsUI() {
    // Update UI to reflect current keyboard/mouse permissions
    // This would update toggle states if we had permission controls
  }

  updateConnectionStatus(status) {
    if (!this.elements.connectionStatus) return;

    const statusMap = {
      'connected': { text: 'Connected', class: 'connected' },
      'connecting': { text: 'Connecting...', class: 'connecting' },
      'disconnected': { text: 'Disconnected', class: 'disconnected' },
      'checking': { text: 'Connecting...', class: 'connecting' },
      'completed': { text: 'Connected', class: 'connected' },
      'failed': { text: 'Failed', class: 'error' },
      'error': { text: 'Error', class: 'error' }
    };

    const info = statusMap[status] || { text: status, class: '' };
    this.elements.connectionStatus.textContent = info.text;
    this.elements.connectionStatus.className = `connection-status ${info.class}`;
  }

  toggleSidebar() {
    this.elements.sidebar?.classList.toggle('open');
  }

  async copyRoomCode() {
    if (!this.roomCode) return;

    try {
      await navigator.clipboard.writeText(this.roomCode);
      this.showNotification('Room code copied!');
    } catch (err) {
      console.error('Failed to copy:', err);
    }
  }

  toggleFullscreen() {
    if (document.fullscreenElement) {
      document.exitFullscreen();
    } else {
      this.elements.videoContainer?.requestFullscreen();
    }
  }

  showLoading(message) {
    // Show loading indicator
    if (this.elements.createRoomBtn) {
      this.elements.createRoomBtn.disabled = true;
      this.elements.createRoomBtn.textContent = message;
    }
    if (this.elements.joinRoomBtn) {
      this.elements.joinRoomBtn.disabled = true;
    }
  }

  hideLoading() {
    if (this.elements.createRoomBtn) {
      this.elements.createRoomBtn.disabled = false;
      this.elements.createRoomBtn.textContent = 'Create Room';
    }
    if (this.elements.joinRoomBtn) {
      this.elements.joinRoomBtn.disabled = false;
    }
  }

  showError(message) {
    // Simple error display
    alert(message);
  }

  showNotification(message) {
    // Create toast notification
    const toast = document.createElement('div');
    toast.className = 'toast';
    toast.textContent = message;
    document.body.appendChild(toast);

    setTimeout(() => {
      toast.classList.add('show');
    }, 10);

    setTimeout(() => {
      toast.classList.remove('show');
      setTimeout(() => toast.remove(), 300);
    }, 3000);
  }

  handleSignalingError(msg) {
    this.hideLoading();
    this.showError(msg.message || 'An error occurred');
  }

  // ============== Quality Settings ==============

  applyQualitySettings() {
    if (!this.isHost) return;

    const bitrate = parseInt(this.elements.bitrateSlider?.value || '10', 10);
    const framerate = parseInt(this.elements.framerateSelect?.value || '60', 10);
    const resolution = this.elements.resolutionSelect?.value || '1080';

    // Map resolution string to actual dimensions
    const resolutionMap = {
      '720': { width: 1280, height: 720 },
      '1080': { width: 1920, height: 1080 },
      '1440': { width: 2560, height: 1440 },
      '4k': { width: 3840, height: 2160 }
    };

    const dims = resolutionMap[resolution] || resolutionMap['1080'];

    // Send quality settings to server via signaling
    this.sendSignaling('set_quality', {
      bitrate: bitrate * 1000, // Convert Mbps to kbps
      framerate: framerate,
      width: dims.width,
      height: dims.height
    });

    this.showNotification('Applying quality settings...');
  }

  setGuestKeyboardPermission(enabled) {
    if (!this.isHost) return;

    // Send permission update for all guest players
    for (const player of this.players) {
      if (player.peer_id !== this.playerId && player.slot > 0) {
        this.sendSignaling('set_guest_keyboard', {
          peer_id: player.peer_id,
          enabled: enabled
        });
      }
    }
    this.showNotification(`Guest keyboard ${enabled ? 'enabled' : 'disabled'}`);
  }

  setGuestMousePermission(enabled) {
    if (!this.isHost) return;

    // Send permission update for all guest players
    for (const player of this.players) {
      if (player.peer_id !== this.playerId && player.slot > 0) {
        this.sendSignaling('set_guest_mouse', {
          peer_id: player.peer_id,
          enabled: enabled
        });
      }
    }
    this.showNotification(`Guest mouse ${enabled ? 'enabled' : 'disabled'}`);
  }

  handleQualityUpdated(msg) {
    // Server confirmed quality settings were applied
    if (msg.success) {
      this.showNotification('Quality settings applied');

      // Update UI to reflect actual values
      if (msg.bitrate && this.elements.bitrateSlider) {
        const mbps = Math.round(msg.bitrate / 1000);
        this.elements.bitrateSlider.value = mbps;
        if (this.elements.bitrateValue) {
          this.elements.bitrateValue.textContent = mbps;
        }
      }
      if (msg.framerate && this.elements.framerateSelect) {
        this.elements.framerateSelect.value = msg.framerate;
      }
      if (msg.width && msg.height && this.elements.resolutionSelect) {
        // Map dimensions back to resolution option
        if (msg.height <= 720) this.elements.resolutionSelect.value = '720';
        else if (msg.height <= 1080) this.elements.resolutionSelect.value = '1080';
        else if (msg.height <= 1440) this.elements.resolutionSelect.value = '1440';
        else this.elements.resolutionSelect.value = '4k';
      }
    } else {
      this.showNotification(msg.error || 'Failed to apply quality settings');
    }
  }

  // ============== Lifecycle ==============

  handleVisibilityChange() {
    if (document.hidden) {
      // Page hidden, pause gamepad polling to save resources
      this.stopGamepadPolling();
    } else {
      // Page visible, resume
      if (this.playerSlot > 0) {
        this.startGamepadPolling();
      }
    }
  }

  handleDisconnect() {
    this.cleanup();
    this.showStartOverlay();
    this.showError('Connection lost');
  }

  cleanup() {
    // Stop polling
    this.stopGamepadPolling();
    this.stopStatsPolling();

    // Release pointer lock
    if (document.pointerLockElement) {
      document.exitPointerLock();
    }

    // Close data channel
    if (this.dataChannel) {
      this.dataChannel.close();
      this.dataChannel = null;
    }

    // Close peer connection
    if (this.pc) {
      this.pc.close();
      this.pc = null;
    }

    // Close WebSocket
    if (this.ws) {
      this.ws.close();
      this.ws = null;
    }

    // Reset state
    this.roomCode = null;
    this.playerId = null;
    this.playerSlot = 0;
    this.isHost = false;
    this.players = [];
    this.gamepads.clear();
    this.lastGamepadState.clear();

    // Clear video
    if (this.elements.videoElement) {
      this.elements.videoElement.srcObject = null;
    }
  }
}

// Initialize when DOM is ready and auto-join
document.addEventListener('DOMContentLoaded', () => {
  window.sunshineWebRTC = new SunshineWebRTC();
  // Auto-join after a short delay to ensure everything is initialized
  setTimeout(() => {
    window.sunshineWebRTC.autoJoin();
  }, 100);
});
