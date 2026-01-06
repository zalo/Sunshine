# Sunshine Discord Activity

Stream games to Discord using Sunshine as a Discord Activity. This enables multiplayer game streaming directly within Discord voice channels, text channels, or DMs.

## Overview

This deployment creates a Discord Activity that:
- Embeds Sunshine's WebRTC streaming in Discord's iframe sandbox
- Uses the Discord Embedded App SDK for authentication
- Shows Discord user avatars and names as participants
- Supports gamepad, keyboard, and mouse input
- Works on Discord desktop, web, and mobile clients

## Prerequisites

1. **Modal Account**: Sign up at [modal.com](https://modal.com)
2. **Discord Developer Account**: Access the [Discord Developer Portal](https://discord.com/developers/applications)
3. **Pre-built Sunshine**: Run `cmake --build build` in the Sunshine root directory

## Setup Instructions

### 1. Create Discord Application

1. Go to [Discord Developer Portal](https://discord.com/developers/applications)
2. Click **New Application** and give it a name
3. Navigate to **OAuth2** > **General**
   - Copy the **Client ID**
   - Copy or reset the **Client Secret**

### 2. Enable Activities

1. In your Discord app, go to **Activities** (in the left sidebar)
2. Toggle **Enable Activities** to ON
3. Under **Supported Platforms**, enable:
   - Desktop
   - Web
   - Mobile (optional)

### 3. Configure Modal Secrets

Create a Modal secret with your Discord credentials:

```bash
modal secret create discord-sunshine-secrets \
  DISCORD_CLIENT_ID=your_client_id_here \
  DISCORD_CLIENT_SECRET=your_client_secret_here
```

### 4. Deploy to Modal

```bash
# Development mode (hot reload)
modal serve modal_deploy/discord_app.py

# Production deployment
modal deploy modal_deploy/discord_app.py
```

After deployment, Modal will provide a URL like:
```
https://<workspace>--sunshine-discord-activity-discord-activity-server.modal.run
```

### 5. Configure URL Mappings in Discord

1. In the Discord Developer Portal, go to **Activities** > **URL Mappings**
2. Add a mapping:
   - **Prefix**: `/`
   - **Target**: Your Modal URL (e.g., `sunshine-discord-activity-discord-activity-server.modal.run`)

3. Set **Root Mapping** to your Modal URL

### 6. Test Your Activity

1. Open Discord (desktop or web client)
2. Join a voice channel or start a DM
3. Click the **Activities** button (rocket ship icon)
4. Search for your app name
5. Launch the activity

## Architecture

```
Discord Client
    │
    ▼
┌───────────────────────────────────────────────────────┐
│  Discord's iframe sandbox (discordsays.com)           │
│  ┌─────────────────────────────────────────────────┐  │
│  │  discord-activity.html                          │  │
│  │  - Discord Embedded App SDK                     │  │
│  │  - WebRTC client (video/audio/input)           │  │
│  └─────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────┘
              │
              │ /.proxy/ (Discord's URL proxy)
              ▼
┌───────────────────────────────────────────────────────┐
│  Modal Container (GPU: L4)                            │
│  ┌─────────────────────────────────────────────────┐  │
│  │  FastAPI Server                                 │  │
│  │  - GET /          → Discord Activity HTML       │  │
│  │  - POST /api/token → OAuth2 token exchange      │  │
│  │  - WS /ws/signaling → WebRTC signaling proxy   │  │
│  └─────────────────────────────────────────────────┘  │
│              │                                        │
│              ▼                                        │
│  ┌─────────────────────────────────────────────────┐  │
│  │  Sunshine Server                                │  │
│  │  - WebRTC media streams                         │  │
│  │  - Input handling (gamepad/keyboard/mouse)     │  │
│  │  - NVENC hardware encoding                      │  │
│  └─────────────────────────────────────────────────┘  │
│              │                                        │
│              ▼                                        │
│  ┌─────────────────────────────────────────────────┐  │
│  │  Xvfb + XFCE Desktop                           │  │
│  │  - Virtual display (:99)                       │  │
│  │  - Chrome browser                              │  │
│  └─────────────────────────────────────────────────┘  │
└───────────────────────────────────────────────────────┘
```

## OAuth2 Flow

1. Discord SDK requests authorization code with scopes `identify` and `guilds`
2. User consents (or auto-approves if previously authorized)
3. Client sends code to `/api/token`
4. Server exchanges code for access token with Discord API
5. Client authenticates with Discord SDK using access token
6. User info (username, avatar) is available for the session

## Features

### Multiplayer Support
- Multiple Discord users can join the same activity
- Host controls stream quality and guest permissions
- Participants see each other's avatars in the overlay

### Input Support
- **Gamepad**: Full controller support with automatic claiming
- **Keyboard**: Send keyboard input to streamed games
- **Mouse**: Absolute positioning for accurate cursor control
- **Touch**: Mobile-friendly touch-to-click support

### Permissions (Host Controls)
- Enable/disable keyboard input for guests
- Enable/disable mouse input for guests
- Control stream quality (bitrate, resolution, framerate)

## Troubleshooting

### "Discord Client ID not configured"
Ensure the Modal secret is created with the correct name:
```bash
modal secret list
# Should show: discord-sunshine-secrets
```

### Activity not appearing in Discord
1. Check that Activities are enabled in your Discord app
2. Verify URL mappings point to your Modal deployment
3. Make sure the deployment is running (`modal app list`)

### Black screen / No video
1. Check browser console for WebRTC errors
2. Verify Sunshine started correctly in Modal logs
3. Try refreshing the activity

### WebSocket connection failed
1. Ensure the signaling proxy is working
2. Check Modal logs for WebSocket errors
3. Verify the URL mapping includes the `/ws/` path

## Development

### Local Testing

For local development without Modal:

1. Run Sunshine locally
2. Update `discord-activity.js` to use local WebSocket URL
3. Use Discord's local testing tools

### Customization

- **Theme**: Edit CSS variables in `discord-activity.html`
- **Participant display**: Modify `updateParticipantOverlay()` in JS
- **Input handling**: Customize in the input handling section

## Files

```
modal_deploy/discord_activity/
├── discord-activity.html   # Main HTML page (Discord SDK + UI)
├── discord-activity.js     # Client-side WebRTC + Discord integration
└── README.md              # This file

modal_deploy/
├── discord_app.py         # Modal deployment (OAuth + proxy + Sunshine)
└── app.py                 # Original Modal deployment (non-Discord)
```

## Resources

- [Discord Activities Overview](https://discord.com/developers/docs/activities/overview)
- [Discord Embedded App SDK](https://github.com/discord/embedded-app-sdk)
- [Modal Documentation](https://modal.com/docs)
- [Sunshine Documentation](https://docs.lizardbyte.dev/projects/sunshine/)
