/**
 * @file src/input.h
 * @brief Declarations for gamepad, keyboard, and mouse input handling.
 */
#pragma once

// standard includes
#include <functional>

// local includes
#include "platform/common.h"
#include "thread_safe.h"

namespace input {
  struct input_t;

  void print(void *input);
  void reset(std::shared_ptr<input_t> &input);
  void passthrough(std::shared_ptr<input_t> &input, std::vector<std::uint8_t> &&input_data);

  [[nodiscard]] std::unique_ptr<platf::deinit_t> init();

  bool probe_gamepads();

  std::shared_ptr<input_t> alloc(safe::mail_t mail);

  struct touch_port_t: public platf::touch_port_t {
    int env_width, env_height;

    // Offset x and y coordinates of the client
    float client_offsetX, client_offsetY;

    float scalar_inv;

    explicit operator bool() const {
      return width != 0 && height != 0 && env_width != 0 && env_height != 0;
    }
  };

  /**
   * @brief Scale the ellipse axes according to the provided size.
   * @param val The major and minor axis pair.
   * @param rotation The rotation value from the touch/pen event.
   * @param scalar The scalar cartesian coordinate pair.
   * @return The major and minor axis pair.
   */
  std::pair<float, float> scale_client_contact_area(const std::pair<float, float> &val, uint16_t rotation, const std::pair<float, float> &scalar);

  /**
   * @brief Direct input functions for WebRTC (bypasses packet protocol).
   * These are simpler wrappers around platform input functions.
   */

  /**
   * @brief Send a keyboard key event.
   * @param key_code Virtual key code.
   * @param release true for key up, false for key down.
   */
  void keyboard(uint16_t key_code, bool release);

  /**
   * @brief Send relative mouse movement.
   * @param delta_x X movement delta.
   * @param delta_y Y movement delta.
   */
  void mouse_move_rel(int16_t delta_x, int16_t delta_y);

  /**
   * @brief Send absolute mouse position.
   * @param x Absolute X position (0-65535 normalized).
   * @param y Absolute Y position (0-65535 normalized).
   */
  void mouse_move_abs(uint16_t x, uint16_t y);

  /**
   * @brief Send a mouse button event.
   * @param button Button number (1=left, 2=middle, 3=right, 4=x1, 5=x2).
   * @param pressed true for button down, false for button up.
   */
  void mouse_button(uint8_t button, bool pressed);

  /**
   * @brief Send a mouse scroll event.
   * @param amount Scroll amount (positive = up/right, negative = down/left).
   * @param horizontal true for horizontal scroll, false for vertical.
   */
  void mouse_scroll(int16_t amount, bool horizontal = false);
}  // namespace input
