/**
 * @file src/webrtc/input.cpp
 * @brief Input handling implementation for WebRTC.
 */

#include "input.h"

#include <cstring>

#include "peer.h"
#include "room.h"
#include "src/input.h"
#include "src/logging.h"

namespace webrtc {

  InputHandler &
  InputHandler::instance() {
    static InputHandler instance;
    return instance;
  }

  void
  InputHandler::init() {
    BOOST_LOG(info) << "WebRTC input handler initialized";
  }

  void
  InputHandler::process_input(const std::string &peer_id, const std::byte *data, size_t size) {
    if (size < 1) {
      return;
    }

    InputType type = static_cast<InputType>(static_cast<uint8_t>(data[0]));

    BOOST_LOG(debug) << "WebRTC input: received type=" << static_cast<int>(data[0])
                     << " size=" << size << " from peer " << peer_id;

    switch (type) {
      case InputType::GAMEPAD_STATE: {
        // Format: type(1) | gamepad_id(1) | buttons(2) | lt(1) | rt(1) | lx(2) | ly(2) | rx(2) | ry(2) = 14 bytes
        if (size >= 14) {
          GamepadState state;
          state.gamepad_id = static_cast<uint8_t>(data[1]);
          state.buttons = static_cast<uint8_t>(data[2]) | (static_cast<uint8_t>(data[3]) << 8);
          state.left_trigger = static_cast<uint8_t>(data[4]);
          state.right_trigger = static_cast<uint8_t>(data[5]);
          state.left_stick_x = static_cast<int16_t>(static_cast<uint8_t>(data[6]) | (static_cast<uint8_t>(data[7]) << 8));
          state.left_stick_y = static_cast<int16_t>(static_cast<uint8_t>(data[8]) | (static_cast<uint8_t>(data[9]) << 8));
          state.right_stick_x = static_cast<int16_t>(static_cast<uint8_t>(data[10]) | (static_cast<uint8_t>(data[11]) << 8));
          state.right_stick_y = static_cast<int16_t>(static_cast<uint8_t>(data[12]) | (static_cast<uint8_t>(data[13]) << 8));
          process_gamepad(peer_id, state);
        }
        break;
      }

      case InputType::KEYBOARD_KEY: {
        // Format: type(1) | key_code(2) | modifiers(1) | pressed(1) = 5 bytes
        if (size >= 5) {
          KeyboardEvent event;
          event.key_code = static_cast<uint8_t>(data[1]) | (static_cast<uint8_t>(data[2]) << 8);
          event.modifiers = static_cast<uint8_t>(data[3]);
          event.pressed = static_cast<uint8_t>(data[4]) != 0;
          process_keyboard(peer_id, event);
        }
        break;
      }

      case InputType::MOUSE_MOVE: {
        // Format: type(1) | flags(1) | x(2) | y(2) = 6 bytes
        // flags: 0x01 = absolute mode, 0x00 = relative mode
        if (size >= 6) {
          MouseMoveEvent event;
          uint8_t flags = static_cast<uint8_t>(data[1]);
          event.is_absolute = (flags & 0x01) != 0;

          if (event.is_absolute) {
            // Absolute mode: x/y are 0-65535 normalized coordinates
            event.abs_x = static_cast<uint16_t>(static_cast<uint8_t>(data[2]) | (static_cast<uint8_t>(data[3]) << 8));
            event.abs_y = static_cast<uint16_t>(static_cast<uint8_t>(data[4]) | (static_cast<uint8_t>(data[5]) << 8));
            event.delta_x = 0;
            event.delta_y = 0;
          }
          else {
            // Relative mode: x/y are signed deltas
            event.delta_x = static_cast<int16_t>(static_cast<uint8_t>(data[2]) | (static_cast<uint8_t>(data[3]) << 8));
            event.delta_y = static_cast<int16_t>(static_cast<uint8_t>(data[4]) | (static_cast<uint8_t>(data[5]) << 8));
            event.abs_x = 0;
            event.abs_y = 0;
          }
          process_mouse_move(peer_id, event);
        }
        break;
      }

      case InputType::MOUSE_BUTTON: {
        // Format: type(1) | button(1) | pressed(1) = 3 bytes
        if (size >= 3) {
          MouseButtonEvent event;
          event.button = static_cast<uint8_t>(data[1]);
          event.pressed = static_cast<uint8_t>(data[2]) != 0;
          process_mouse_button(peer_id, event);
        }
        break;
      }

      case InputType::MOUSE_SCROLL: {
        // Format: type(1) | reserved(1) | delta_x(2) | delta_y(2) = 6 bytes
        if (size >= 6) {
          MouseScrollEvent event;
          event.delta_x = static_cast<int16_t>(static_cast<uint8_t>(data[2]) | (static_cast<uint8_t>(data[3]) << 8));
          event.delta_y = static_cast<int16_t>(static_cast<uint8_t>(data[4]) | (static_cast<uint8_t>(data[5]) << 8));
          event.high_resolution = true;
          process_mouse_scroll(peer_id, event);
        }
        break;
      }

      default:
        BOOST_LOG(warning) << "Unknown input type: " << static_cast<int>(type);
        break;
    }
  }

  void
  InputHandler::process_gamepad(const std::string &peer_id, const GamepadState &state) {
    // Find the room and get the server gamepad slot
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      return;
    }

    // Check if this peer is a player (not spectator)
    auto player = room->get_player(peer_id);
    if (!player || player->is_spectator) {
      return;
    }

    // Get the server gamepad slot for this browser gamepad
    int server_slot = room->get_gamepad_slot(peer_id, state.gamepad_id);
    if (server_slot < 0) {
      // Gamepad not claimed - auto-claim it
      server_slot = room->claim_gamepad(peer_id, state.gamepad_id);
      if (server_slot < 0) {
        BOOST_LOG(warning) << "Failed to claim gamepad for peer " << peer_id;
        return;
      }
    }

    // Build and send gamepad packet to Sunshine's input system
    auto packet = build_gamepad_packet(server_slot, state);
    send_to_input_system(packet);
  }

  void
  InputHandler::process_keyboard(const std::string &peer_id, const KeyboardEvent &event) {
    // Check if peer has keyboard permission
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      BOOST_LOG(debug) << "WebRTC input: keyboard event from peer " << peer_id << " - no room found";
      return;
    }

    if (!room->can_use_keyboard(peer_id)) {
      BOOST_LOG(debug) << "WebRTC input: keyboard event from peer " << peer_id << " - no permission";
      return;
    }

    BOOST_LOG(debug) << "WebRTC input: keyboard key=" << event.key_code
                     << " pressed=" << event.pressed << " from peer " << peer_id;

    // Call Sunshine's input system directly
    input::keyboard(event.key_code, !event.pressed);
  }

  void
  InputHandler::process_mouse_move(const std::string &peer_id, const MouseMoveEvent &event) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      return;
    }

    if (!room->can_use_mouse(peer_id)) {
      return;
    }

    // Call Sunshine's input system directly
    if (event.is_absolute) {
      input::mouse_move_abs(event.abs_x, event.abs_y);
    }
    else {
      input::mouse_move_rel(event.delta_x, event.delta_y);
    }
  }

  void
  InputHandler::process_mouse_button(const std::string &peer_id, const MouseButtonEvent &event) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      return;
    }

    if (!room->can_use_mouse(peer_id)) {
      return;
    }

    BOOST_LOG(debug) << "WebRTC input: mouse button=" << (int) event.button
                     << " pressed=" << event.pressed << " from peer " << peer_id;

    // Button mapping: browser (0=left, 1=middle, 2=right) -> Sunshine (1=left, 2=middle, 3=right)
    input::mouse_button(event.button + 1, event.pressed);
  }

  void
  InputHandler::process_mouse_scroll(const std::string &peer_id, const MouseScrollEvent &event) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      return;
    }

    if (!room->can_use_mouse(peer_id)) {
      return;
    }

    // Send vertical scroll
    if (event.delta_y != 0) {
      input::mouse_scroll(event.delta_y, false);
    }

    // Send horizontal scroll
    if (event.delta_x != 0) {
      input::mouse_scroll(event.delta_x, true);
    }
  }

  void
  InputHandler::send_rumble(const std::string &peer_id,
    int gamepad_slot,
    uint16_t low_freq,
    uint16_t high_freq,
    uint16_t duration_ms) {
    auto peer = PeerManager::instance().find_peer(peer_id);
    if (!peer) {
      return;
    }

    // Build rumble message
    std::vector<std::byte> message(1 + sizeof(uint8_t) + sizeof(uint16_t) * 3);

    message[0] = std::byte{static_cast<uint8_t>(InputType::GAMEPAD_RUMBLE)};
    message[1] = std::byte{static_cast<uint8_t>(gamepad_slot)};

    // Low frequency (little-endian)
    message[2] = std::byte{static_cast<uint8_t>(low_freq & 0xFF)};
    message[3] = std::byte{static_cast<uint8_t>((low_freq >> 8) & 0xFF)};

    // High frequency (little-endian)
    message[4] = std::byte{static_cast<uint8_t>(high_freq & 0xFF)};
    message[5] = std::byte{static_cast<uint8_t>((high_freq >> 8) & 0xFF)};

    // Duration (little-endian)
    message[6] = std::byte{static_cast<uint8_t>(duration_ms & 0xFF)};
    message[7] = std::byte{static_cast<uint8_t>((duration_ms >> 8) & 0xFF)};

    peer->send_binary("input", message);
  }

  std::vector<uint8_t>
  InputHandler::build_gamepad_packet(int server_slot, const GamepadState &state) {
    // Build packet compatible with Sunshine's input::passthrough()
    // This matches the MULTI_CONTROLLER packet format

    std::vector<uint8_t> packet;
    packet.reserve(24);

    // Packet type for multi-controller (0x0D)
    packet.push_back(0x0D);

    // Header size (little-endian, 2 bytes)
    uint16_t header_size = 22;
    packet.push_back(header_size & 0xFF);
    packet.push_back((header_size >> 8) & 0xFF);

    // Controller number
    packet.push_back(static_cast<uint8_t>(server_slot));

    // Active gamepad mask (bit set for active controllers)
    uint16_t active_mask = 1 << server_slot;
    packet.push_back(active_mask & 0xFF);
    packet.push_back((active_mask >> 8) & 0xFF);

    // Mid value (not used, but required)
    packet.push_back(0x7F);
    packet.push_back(0x7F);

    // Button flags (little-endian, 2 bytes)
    packet.push_back(state.buttons & 0xFF);
    packet.push_back((state.buttons >> 8) & 0xFF);

    // Left trigger
    packet.push_back(state.left_trigger);

    // Right trigger
    packet.push_back(state.right_trigger);

    // Left stick X (little-endian, 2 bytes)
    packet.push_back(state.left_stick_x & 0xFF);
    packet.push_back((state.left_stick_x >> 8) & 0xFF);

    // Left stick Y (little-endian, 2 bytes)
    packet.push_back(state.left_stick_y & 0xFF);
    packet.push_back((state.left_stick_y >> 8) & 0xFF);

    // Right stick X (little-endian, 2 bytes)
    packet.push_back(state.right_stick_x & 0xFF);
    packet.push_back((state.right_stick_x >> 8) & 0xFF);

    // Right stick Y (little-endian, 2 bytes)
    packet.push_back(state.right_stick_y & 0xFF);
    packet.push_back((state.right_stick_y >> 8) & 0xFF);

    return packet;
  }

  std::vector<uint8_t>
  InputHandler::build_keyboard_packet(const KeyboardEvent &event) {
    // Build packet for keyboard input
    // Matches Sunshine's KEYBOARD packet format

    std::vector<uint8_t> packet;
    packet.reserve(8);

    // Packet type (0x0A for key)
    packet.push_back(event.pressed ? 0x0A : 0x0B);

    // Header size (little-endian)
    uint16_t header_size = 6;
    packet.push_back(header_size & 0xFF);
    packet.push_back((header_size >> 8) & 0xFF);

    // Key code (little-endian)
    packet.push_back(event.key_code & 0xFF);
    packet.push_back((event.key_code >> 8) & 0xFF);

    // Modifiers
    packet.push_back(event.modifiers);

    // Flags (0 for single key event)
    packet.push_back(0);
    packet.push_back(0);

    return packet;
  }

  std::vector<uint8_t>
  InputHandler::build_mouse_move_packet(const MouseMoveEvent &event) {
    std::vector<uint8_t> packet;

    if (event.is_absolute) {
      // Absolute mouse position (0x08)
      packet.reserve(10);
      packet.push_back(0x08);

      // Header size
      uint16_t header_size = 8;
      packet.push_back(header_size & 0xFF);
      packet.push_back((header_size >> 8) & 0xFF);

      // X position (little-endian)
      packet.push_back(event.abs_x & 0xFF);
      packet.push_back((event.abs_x >> 8) & 0xFF);

      // Y position (little-endian)
      packet.push_back(event.abs_y & 0xFF);
      packet.push_back((event.abs_y >> 8) & 0xFF);

      // Reference dimensions (unused, set to 0)
      packet.push_back(0);
      packet.push_back(0);
      packet.push_back(0);
      packet.push_back(0);
    }
    else {
      // Relative mouse movement (0x07)
      packet.reserve(8);
      packet.push_back(0x07);

      // Header size
      uint16_t header_size = 6;
      packet.push_back(header_size & 0xFF);
      packet.push_back((header_size >> 8) & 0xFF);

      // Delta X (little-endian, signed)
      packet.push_back(event.delta_x & 0xFF);
      packet.push_back((event.delta_x >> 8) & 0xFF);

      // Delta Y (little-endian, signed)
      packet.push_back(event.delta_y & 0xFF);
      packet.push_back((event.delta_y >> 8) & 0xFF);
    }

    return packet;
  }

  std::vector<uint8_t>
  InputHandler::build_mouse_button_packet(const MouseButtonEvent &event) {
    std::vector<uint8_t> packet;
    packet.reserve(6);

    // Packet type (0x05 for button down, 0x06 for button up)
    packet.push_back(event.pressed ? 0x05 : 0x06);

    // Header size
    uint16_t header_size = 4;
    packet.push_back(header_size & 0xFF);
    packet.push_back((header_size >> 8) & 0xFF);

    // Button (1=left, 2=middle, 3=right, 4=x1, 5=x2)
    packet.push_back(event.button + 1);

    return packet;
  }

  std::vector<uint8_t>
  InputHandler::build_mouse_scroll_packet(const MouseScrollEvent &event) {
    std::vector<uint8_t> packet;

    if (event.high_resolution) {
      // High-resolution scroll (0x09)
      packet.reserve(10);
      packet.push_back(0x09);

      // Header size
      uint16_t header_size = 8;
      packet.push_back(header_size & 0xFF);
      packet.push_back((header_size >> 8) & 0xFF);

      // Scroll amount X (little-endian, signed)
      packet.push_back(event.delta_x & 0xFF);
      packet.push_back((event.delta_x >> 8) & 0xFF);

      // Scroll amount Y (little-endian, signed)
      packet.push_back(event.delta_y & 0xFF);
      packet.push_back((event.delta_y >> 8) & 0xFF);

      // High-res flag
      packet.push_back(1);
      packet.push_back(0);
    }
    else {
      // Normal scroll (0x09)
      packet.reserve(8);
      packet.push_back(0x09);

      // Header size
      uint16_t header_size = 6;
      packet.push_back(header_size & 0xFF);
      packet.push_back((header_size >> 8) & 0xFF);

      // Scroll amount (Y only for normal scroll, in "clicks")
      int16_t scroll_clicks = event.delta_y / 120;  // 120 units per "click"
      packet.push_back(scroll_clicks & 0xFF);
      packet.push_back((scroll_clicks >> 8) & 0xFF);

      // High-res flag
      packet.push_back(0);
      packet.push_back(0);
    }

    return packet;
  }

  void
  InputHandler::send_to_input_system(const std::vector<uint8_t> &packet) {
    // Forward to Sunshine's input system
    // This integrates with input::passthrough()

    // Get the input context (this will be set up when a stream is active)
    // For now, we call input::passthrough directly

    // Note: input::passthrough expects a shared_ptr<input_t> and the input data
    // The actual integration will happen in the main Sunshine code

    // Placeholder - the actual implementation will be integrated with stream.cpp
    BOOST_LOG(debug) << "Input packet: " << packet.size() << " bytes";
  }

}  // namespace webrtc
