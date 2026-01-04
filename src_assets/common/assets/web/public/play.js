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
      videoContainer: document.getElementById('videoContainer'),
      videoElement: document.getElementById('videoElement'),
      sidebar: document.getElementById('sidebar'),
      sidebarToggle: document.getElementById('sidebarToggle'),
      roomCodeDisplay: document.getElementById('roomCodeDisplay'),
      copyCodeBtn: document.getElementById('copyCodeBtn'),
      playerList: document.getElementById('playerList'),
      joinAsPlayerBtn: document.getElementById('joinAsPlayerBtn'),
      permissionsPanel: document.getElementById('permissionsPanel'),
      statsBitrate: document.getElementById('statsBitrate'),
      statsFps: document.getElementById('statsFps'),
      statsRtt: document.getElementById('statsRtt'),
      statsPacketLoss: document.getElementById('statsPacketLoss'),
      gamepadIndicator: document.getElementById('gamepadIndicator'),
      fullscreenBtn: document.getElementById('fullscreenBtn'),
      leaveBtn: document.getElementById('leaveBtn'),
      connectionStatus: document.getElementById('connectionStatus')
    };

    this.bindEvents();
    this.loadSavedName();
  }

  bindEvents() {
    // Start overlay events
    this.elements.createRoomBtn?.addEventListener('click', () => this.createRoom());
    this.elements.joinRoomBtn?.addEventListener('click', () => this.joinRoom());
    this.elements.joinCodeInput?.addEventListener('keypress', (e) => {
      if (e.key === 'Enter') this.joinRoom();
    });

    // Format room code input (uppercase, max 6 chars)
    this.elements.joinCodeInput?.addEventListener('input', (e) => {
      e.target.value = e.target.value.toUpperCase().replace(/[^A-Z0-9]/g, '').slice(0, 6);
    });

    // Sidebar events
    this.elements.sidebarToggle?.addEventListener('click', () => this.toggleSidebar());
    this.elements.copyCodeBtn?.addEventListener('click', () => this.copyRoomCode());
    this.elements.joinAsPlayerBtn?.addEventListener('click', () => this.requestJoinAsPlayer());
    this.elements.fullscreenBtn?.addEventListener('click', () => this.toggleFullscreen());
    this.elements.leaveBtn?.addEventListener('click', () => this.leaveRoom());

    // Video events
    this.elements.videoElement?.addEventListener('click', () => this.requestPointerLock());

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
      default:
        console.warn('Unknown signaling message type:', msg.type);
    }
  }

  // ============== Room Management ==============

  async createRoom() {
    const name = this.elements.playerNameInput?.value.trim() || 'Player';
    this.savePlayerName(name);

    try {
      this.showLoading('Creating room...');
      await this.connectSignaling();
      this.sendSignaling('create_room', { name });
    } catch (err) {
      this.hideLoading();
      this.showError('Failed to connect to server');
    }
  }

  async joinRoom() {
    const code = this.elements.joinCodeInput?.value.trim().toUpperCase();
    if (!code || code.length !== 6) {
      this.showError('Please enter a valid 6-character room code');
      return;
    }

    const name = this.elements.playerNameInput?.value.trim() || 'Player';
    this.savePlayerName(name);

    try {
      this.showLoading('Joining room...');
      await this.connectSignaling();
      this.sendSignaling('join_room', { room_code: code, name });
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

    // Initialize peer connection immediately - server sends SDP right after room creation
    this.initPeerConnection();

    this.showStreamUI();
    this.updateRoomUI();
  }

  handleRoomJoined(msg) {
    this.hideLoading();
    this.roomCode = msg.room_code;
    this.playerId = msg.peer_id;
    this.playerSlot = msg.slot || 0;
    this.isHost = msg.is_host || false;
    this.players = msg.players || [];

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
    // Server is ready to send stream, initialize WebRTC
    if (msg.ice_servers) {
      this.config.iceServers = msg.ice_servers;
    }
    this.initPeerConnection();
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
      if (e.track.kind === 'video') {
        this.elements.videoElement.srcObject = e.streams[0];
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
    if (!this.pointerLocked) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    e.preventDefault();
    this.sendKeyEvent(e.code, true, e.repeat);
  }

  handleKeyUp(e) {
    if (!this.keyboardEnabled || this.playerSlot === 0) return;
    if (!this.pointerLocked) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    e.preventDefault();
    this.sendKeyEvent(e.code, false, false);
  }

  sendKeyEvent(code, pressed, repeat) {
    const buffer = new ArrayBuffer(4);
    const view = new DataView(buffer);

    view.setUint8(0, 0x02); // Type: keyboard
    view.setUint8(1, pressed ? 1 : 0);
    view.setUint16(2, this.keyCodeToScancode(code), true);

    this.dataChannel.send(buffer);
  }

  keyCodeToScancode(code) {
    // Map JavaScript key codes to USB HID scancodes
    const mapping = {
      'KeyA': 0x04, 'KeyB': 0x05, 'KeyC': 0x06, 'KeyD': 0x07,
      'KeyE': 0x08, 'KeyF': 0x09, 'KeyG': 0x0A, 'KeyH': 0x0B,
      'KeyI': 0x0C, 'KeyJ': 0x0D, 'KeyK': 0x0E, 'KeyL': 0x0F,
      'KeyM': 0x10, 'KeyN': 0x11, 'KeyO': 0x12, 'KeyP': 0x13,
      'KeyQ': 0x14, 'KeyR': 0x15, 'KeyS': 0x16, 'KeyT': 0x17,
      'KeyU': 0x18, 'KeyV': 0x19, 'KeyW': 0x1A, 'KeyX': 0x1B,
      'KeyY': 0x1C, 'KeyZ': 0x1D,
      'Digit1': 0x1E, 'Digit2': 0x1F, 'Digit3': 0x20, 'Digit4': 0x21,
      'Digit5': 0x22, 'Digit6': 0x23, 'Digit7': 0x24, 'Digit8': 0x25,
      'Digit9': 0x26, 'Digit0': 0x27,
      'Enter': 0x28, 'Escape': 0x29, 'Backspace': 0x2A, 'Tab': 0x2B,
      'Space': 0x2C, 'Minus': 0x2D, 'Equal': 0x2E, 'BracketLeft': 0x2F,
      'BracketRight': 0x30, 'Backslash': 0x31, 'Semicolon': 0x33,
      'Quote': 0x34, 'Backquote': 0x35, 'Comma': 0x36, 'Period': 0x37,
      'Slash': 0x38, 'CapsLock': 0x39,
      'F1': 0x3A, 'F2': 0x3B, 'F3': 0x3C, 'F4': 0x3D,
      'F5': 0x3E, 'F6': 0x3F, 'F7': 0x40, 'F8': 0x41,
      'F9': 0x42, 'F10': 0x43, 'F11': 0x44, 'F12': 0x45,
      'PrintScreen': 0x46, 'ScrollLock': 0x47, 'Pause': 0x48,
      'Insert': 0x49, 'Home': 0x4A, 'PageUp': 0x4B,
      'Delete': 0x4C, 'End': 0x4D, 'PageDown': 0x4E,
      'ArrowRight': 0x4F, 'ArrowLeft': 0x50, 'ArrowDown': 0x51, 'ArrowUp': 0x52,
      'NumLock': 0x53,
      'ShiftLeft': 0xE1, 'ShiftRight': 0xE5,
      'ControlLeft': 0xE0, 'ControlRight': 0xE4,
      'AltLeft': 0xE2, 'AltRight': 0xE6,
      'MetaLeft': 0xE3, 'MetaRight': 0xE7
    };

    return mapping[code] || 0;
  }

  // ============== Mouse Input ==============

  handleMouseMove(e) {
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.pointerLocked) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    this.sendMouseMove(e.movementX, e.movementY);
  }

  handleMouseButton(e, pressed) {
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.pointerLocked) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    e.preventDefault();
    this.sendMouseButton(e.button, pressed);
  }

  handleMouseWheel(e) {
    if (!this.mouseEnabled || this.playerSlot === 0) return;
    if (!this.pointerLocked) return;
    if (!this.dataChannel || this.dataChannel.readyState !== 'open') return;

    e.preventDefault();
    this.sendMouseScroll(e.deltaX, e.deltaY);
  }

  sendMouseMove(dx, dy) {
    const buffer = new ArrayBuffer(6);
    const view = new DataView(buffer);

    view.setUint8(0, 0x03); // Type: mouse move
    view.setUint8(1, 0); // Reserved
    view.setInt16(2, dx, true);
    view.setInt16(4, dy, true);

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

    // Show/hide join as player button
    if (this.elements.joinAsPlayerBtn) {
      this.elements.joinAsPlayerBtn.style.display = this.playerSlot === 0 ? 'block' : 'none';
    }

    // Show/hide permissions panel (host only)
    if (this.elements.permissionsPanel) {
      this.elements.permissionsPanel.style.display = this.isHost ? 'block' : 'none';
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

// Initialize when DOM is ready
document.addEventListener('DOMContentLoaded', () => {
  window.sunshineWebRTC = new SunshineWebRTC();
});
