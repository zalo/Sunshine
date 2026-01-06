/**
 * @file src/platform/linux/input/inputtino_common.h
 * @brief Declarations for inputtino common input handling.
 */
#pragma once

// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>
#include <cstdlib>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf {

  using joypads_t = std::variant<inputtino::XboxOneJoypad, inputtino::SwitchJoypad, inputtino::PS5Joypad>;

  struct joypad_state {
    std::unique_ptr<joypads_t> joypad;
    gamepad_feedback_msg_t last_rumble;
    gamepad_feedback_msg_t last_rgb_led;
  };

  /**
   * @brief Check if xdotool should be used for input instead of uinput.
   *
   * Returns true if:
   * - SUNSHINE_USE_XDOTOOL environment variable is set to "1"
   * - OR we detect we're running in Xvfb (virtual framebuffer) which doesn't read uinput
   */
  inline bool should_use_xdotool() {
    // Check for explicit override
    const char *env = std::getenv("SUNSHINE_USE_XDOTOOL");
    if (env && std::string(env) == "1") {
      return true;
    }

    // Auto-detect Xvfb by checking if DISPLAY points to a virtual display
    // Xvfb typically uses :99 or similar high display numbers
    const char *display = std::getenv("DISPLAY");
    if (display) {
      std::string disp(display);
      // Check for common Xvfb patterns
      if (disp.find(":99") != std::string::npos ||
          disp.find(":98") != std::string::npos ||
          disp.find(":1") == 0) {  // :1, :10, etc. often virtual
        // Additionally check if we can access /tmp/.X11-unix/X99 (Xvfb socket)
        std::string socket_path = "/tmp/.X11-unix/X" + disp.substr(disp.find(":") + 1);
        // If the socket exists and display is :99, likely Xvfb
        if (disp.find(":99") != std::string::npos) {
          return true;
        }
      }
    }

    return false;
  }

  struct input_raw_t {
    input_raw_t():
        use_xdotool(should_use_xdotool()),
        mouse(inputtino::Mouse::create({
          .name = "Mouse passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        keyboard(inputtino::Keyboard::create({
          .name = "Keyboard passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        gamepads(MAX_GAMEPADS) {
      if (use_xdotool) {
        BOOST_LOG(info) << "Using xdotool for input (Xvfb/virtual display detected)";
      } else {
        if (!mouse) {
          BOOST_LOG(warning) << "Unable to create virtual mouse: " << mouse.getErrorMessage();
        }
        if (!keyboard) {
          BOOST_LOG(warning) << "Unable to create virtual keyboard: " << keyboard.getErrorMessage();
        }
      }
    }

    ~input_raw_t() = default;

    // Flag to use xdotool instead of uinput (for Xvfb environments)
    bool use_xdotool;

    // All devices are wrapped in Result because it might be that we aren't able to create them (ex: udev permission denied)
    inputtino::Result<inputtino::Mouse> mouse;
    inputtino::Result<inputtino::Keyboard> keyboard;

    /**
     * A list of gamepads that are currently connected.
     * The pointer is shared because that state will be shared with background threads that deal with rumble and LED
     */
    std::vector<std::shared_ptr<joypad_state>> gamepads;
  };

  struct client_input_raw_t: public client_input_t {
    client_input_raw_t(input_t &input):
        touch(inputtino::TouchScreen::create({
          .name = "Touch passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })),
        pen(inputtino::PenTablet::create({
          .name = "Pen passthrough",
          .vendor_id = 0xBEEF,
          .product_id = 0xDEAD,
          .version = 0x111,
        })) {
      global = (input_raw_t *) input.get();
      if (!touch) {
        BOOST_LOG(warning) << "Unable to create virtual touch screen: " << touch.getErrorMessage();
      }
      if (!pen) {
        BOOST_LOG(warning) << "Unable to create virtual pen tablet: " << pen.getErrorMessage();
      }
    }

    input_raw_t *global;

    // Device state and handles for pen and touch input must be stored in the per-client
    // input context, because each connected client may be sending their own independent
    // pen/touch events. To maintain separation, we expose separate pen and touch devices
    // for each client.
    inputtino::Result<inputtino::TouchScreen> touch;
    inputtino::Result<inputtino::PenTablet> pen;
  };

  inline float deg2rad(float degree) {
    return degree * (M_PI / 180.f);
  }
}  // namespace platf
