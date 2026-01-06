# Modal Deployment Technical Notes

## Display and GPU Architecture

### Xvfb Frame Rate
Xvfb (X Virtual Framebuffer) does **not have a refresh rate or frame rate**. It's a virtual framebuffer that applications draw to whenever they want - there's no vsync or frame limiting.

The "144 fps" seen in the stream is controlled by **Sunshine's capture rate**, not Xvfb. The actual achieved FPS depends on:
1. How fast applications render to the framebuffer
2. Sunshine's capture and encode speed (typically ~80-85 fps with NVENC on L4 GPU)

### Xvfb Hardware Acceleration
**Xvfb is purely software-rendered** - it runs entirely on CPU with no GPU acceleration for rendering. This is a significant limitation for graphics-intensive applications.

### GPU-Accelerated Display Options

| Option | GPU Rendering | Notes |
|--------|--------------|-------|
| **Xvfb** | No | Simple, works everywhere |
| **Xorg + NVIDIA driver** | Yes | Needs `/dev/dri` and DRM kernel interfaces |
| **VirtualGL + Xvfb** | Yes (OpenGL only) | Intercepts OpenGL calls |
| **EGL headless (GBM)** | Yes | Direct GPU rendering, complex setup |

### Modal Limitations (gVisor)

Modal runs containers on **gVisor**, a container sandbox that lacks kernel interfaces required for NVIDIA X display:

- **NVENC encoding works**: Uses CUDA API which Modal exposes
- **NVIDIA X display fails**: Needs kernel DRM interface (`/dev/dri`) unavailable in gVisor
- **Xvfb works**: Pure software, no kernel dependencies

Error seen when attempting NVIDIA Xorg on Modal:
```
Xorg failed: ... Linux modal 4.4.0 ... Kernel command line: BOOT_IMAGE=/vmlinuz-4.4.0-gvisor
Falling back to Xvfb (software rendering)
```

### Current Architecture on Modal

```
┌─────────────────────────────────────────────────────┐
│                    Modal (gVisor)                   │
├─────────────────────────────────────────────────────┤
│  Desktop Apps (Chrome, XFCE)                        │
│         │                                           │
│         ▼ Software Rendering (CPU)                  │
│  ┌─────────────┐                                    │
│  │    Xvfb     │  Virtual Framebuffer               │
│  └─────────────┘                                    │
│         │                                           │
│         ▼ Screen Capture                            │
│  ┌─────────────┐                                    │
│  │  Sunshine   │  Captures X11 via XTEST/XShm      │
│  └─────────────┘                                    │
│         │                                           │
│         ▼ Hardware Encoding (GPU)                   │
│  ┌─────────────┐                                    │
│  │   NVENC     │  H.264/HEVC/AV1 encoding          │
│  └─────────────┘                                    │
│         │                                           │
│         ▼ WebRTC                                    │
│  ┌─────────────┐                                    │
│  │   Browser   │  Client receives stream            │
│  └─────────────┘                                    │
└─────────────────────────────────────────────────────┘
```

### For True GPU-Accelerated Desktop Rendering

If GPU-accelerated desktop rendering is needed, use platforms with bare-metal or full VM access:
- AWS G4/G5 instances
- GCP GPU VMs
- Azure NV-series
- Any bare-metal GPU server

These provide `/dev/dri` access allowing NVIDIA X driver to work.

## Implementation Details

### start_display() Function

The `start_display()` function in `app.py` attempts NVIDIA Xorg first, then falls back to Xvfb:

1. Creates `/etc/X11/xorg.conf` with NVIDIA headless configuration
2. Attempts to start Xorg with NVIDIA driver
3. Verifies with `xdpyinfo`
4. Falls back to Xvfb if Xorg fails

This ensures the deployment works on any platform - using GPU acceleration where available.
