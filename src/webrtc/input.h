/**
 * @file src/webrtc/input.h
 * @brief Input handling from WebRTC data channels.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace webrtc {

  // Forward declarations
  class Peer;
  class Room;

  /**
   * @brief Input event types from browser clients.
   */
  enum class InputType : uint8_t {
    GAMEPAD_STATE = 0x01,
    GAMEPAD_RUMBLE = 0x02,
    KEYBOARD_KEY = 0x10,
    MOUSE_MOVE = 0x20,
    MOUSE_BUTTON = 0x21,
    MOUSE_SCROLL = 0x22,
    TOUCH = 0x30
  };

  /**
   * @brief Gamepad button flags (matches Moonlight/Sunshine protocol).
   */
  enum GamepadButton : uint16_t {
    DPAD_UP = 0x0001,         ///< D-pad up button
    DPAD_DOWN = 0x0002,       ///< D-pad down button
    DPAD_LEFT = 0x0004,       ///< D-pad left button
    DPAD_RIGHT = 0x0008,      ///< D-pad right button
    START = 0x0010,           ///< Start/Menu button
    BACK = 0x0020,            ///< Back/View button
    LEFT_STICK = 0x0040,      ///< Left stick click (L3)
    RIGHT_STICK = 0x0080,     ///< Right stick click (R3)
    LEFT_SHOULDER = 0x0100,   ///< Left shoulder button (LB)
    RIGHT_SHOULDER = 0x0200,  ///< Right shoulder button (RB)
    HOME = 0x0400,            ///< Home/Guide button
    A = 0x1000,               ///< A/Cross button
    B = 0x2000,               ///< B/Circle button
    X = 0x4000,               ///< X/Square button
    Y = 0x8000                ///< Y/Triangle button
  };

  /**
   * @brief Gamepad state from a browser client.
   */
  struct GamepadState {
    uint8_t gamepad_id;       // Browser gamepad index (0-3)
    uint16_t buttons;         // Button flags
    uint8_t left_trigger;     // 0-255
    uint8_t right_trigger;    // 0-255
    int16_t left_stick_x;     // -32768 to 32767
    int16_t left_stick_y;
    int16_t right_stick_x;
    int16_t right_stick_y;
  };

  /**
   * @brief Keyboard event from a browser client.
   */
  struct KeyboardEvent {
    uint16_t key_code;        // Virtual key code (Windows VK_*)
    uint8_t modifiers;        // Modifier flags (Shift, Ctrl, Alt, Meta)
    bool pressed;             // true = key down, false = key up
  };

  /**
   * @brief Mouse movement event.
   */
  struct MouseMoveEvent {
    int16_t delta_x;          // Relative movement
    int16_t delta_y;
    // Or absolute (if using absolute mode):
    uint16_t abs_x;           // 0-65535 normalized
    uint16_t abs_y;
    bool is_absolute;
  };

  /**
   * @brief Mouse button event.
   */
  struct MouseButtonEvent {
    uint8_t button;           // 0=left, 1=middle, 2=right, 3=x1, 4=x2
    bool pressed;
  };

  /**
   * @brief Mouse scroll event.
   */
  struct MouseScrollEvent {
    int16_t delta_x;          // Horizontal scroll
    int16_t delta_y;          // Vertical scroll (positive = up)
    bool high_resolution;     // True for smooth scrolling
  };

  /**
   * @brief Handles input from WebRTC data channels.
   */
  class InputHandler {
  public:
    static InputHandler &
    instance();

    /**
     * @brief Initialize the input handler.
     */
    void
    init();

    /**
     * @brief Process raw input data from a peer's data channel.
     * @param peer_id The peer sending input.
     * @param data Raw binary input data.
     * @param size Data size.
     */
    void
    process_input(const std::string &peer_id, const std::byte *data, size_t size);

    /**
     * @brief Process a gamepad state update.
     * @param peer_id The peer sending input.
     * @param state The gamepad state.
     */
    void
    process_gamepad(const std::string &peer_id, const GamepadState &state);

    /**
     * @brief Process a keyboard event.
     * @param peer_id The peer sending input.
     * @param event The keyboard event.
     */
    void
    process_keyboard(const std::string &peer_id, const KeyboardEvent &event);

    /**
     * @brief Process a mouse movement event.
     * @param peer_id The peer sending input.
     * @param event The mouse event.
     */
    void
    process_mouse_move(const std::string &peer_id, const MouseMoveEvent &event);

    /**
     * @brief Process a mouse button event.
     * @param peer_id The peer sending input.
     * @param event The mouse button event.
     */
    void
    process_mouse_button(const std::string &peer_id, const MouseButtonEvent &event);

    /**
     * @brief Process a mouse scroll event.
     * @param peer_id The peer sending input.
     * @param event The scroll event.
     */
    void
    process_mouse_scroll(const std::string &peer_id, const MouseScrollEvent &event);

    /**
     * @brief Send rumble feedback to a peer's gamepad.
     * @param peer_id The target peer.
     * @param gamepad_slot The browser gamepad index.
     * @param low_freq Low frequency motor (0-65535).
     * @param high_freq High frequency motor (0-65535).
     * @param duration_ms Duration in milliseconds.
     */
    void
    send_rumble(const std::string &peer_id,
      int gamepad_slot,
      uint16_t low_freq,
      uint16_t high_freq,
      uint16_t duration_ms);

  private:
    InputHandler() = default;
    ~InputHandler() = default;

    // Non-copyable
    InputHandler(const InputHandler &) = delete;
    InputHandler &operator=(const InputHandler &) = delete;

    /**
     * @brief Build input packet for Sunshine's input system.
     */
    std::vector<uint8_t>
    build_gamepad_packet(int server_slot, const GamepadState &state);

    std::vector<uint8_t>
    build_keyboard_packet(const KeyboardEvent &event);

    std::vector<uint8_t>
    build_mouse_move_packet(const MouseMoveEvent &event);

    std::vector<uint8_t>
    build_mouse_button_packet(const MouseButtonEvent &event);

    std::vector<uint8_t>
    build_mouse_scroll_packet(const MouseScrollEvent &event);

    /**
     * @brief Send input packet to Sunshine's input system.
     */
    void
    send_to_input_system(const std::vector<uint8_t> &packet);
  };

}  // namespace webrtc
