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

// X11 includes for XTEST (only when X11 support is enabled)
#ifdef SUNSHINE_BUILD_X11
  #include <X11/Xlib.h>
  #include <X11/extensions/XTest.h>
  #include <X11/keysym.h>
#endif

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

#ifdef SUNSHINE_BUILD_X11
  /**
   * @brief Check if XTEST should be used for input instead of uinput.
   *
   * Returns true if:
   * - SUNSHINE_USE_XTEST environment variable is set to "1"
   * - OR we detect we're running in Xvfb (virtual framebuffer) which doesn't read uinput
   */
  inline bool should_use_xtest() {
    // Check for explicit override
    const char *env = std::getenv("SUNSHINE_USE_XTEST");
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
        // If the socket exists and display is :99, likely Xvfb
        if (disp.find(":99") != std::string::npos) {
          return true;
        }
      }
    }

    return false;
  }
#endif

  struct input_raw_t {
    input_raw_t():
#ifdef SUNSHINE_BUILD_X11
        use_xtest(should_use_xtest()),
        x_display(nullptr),
#endif
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
#ifdef SUNSHINE_BUILD_X11
      if (use_xtest) {
        // Open X11 display connection for XTEST
        x_display = XOpenDisplay(nullptr);
        if (x_display) {
          // Check if XTEST extension is available
          int event_base, error_base, major, minor;
          if (XTestQueryExtension(x_display, &event_base, &error_base, &major, &minor)) {
            BOOST_LOG(info) << "Using X11 XTEST for input (Xvfb/virtual display detected)";
          } else {
            BOOST_LOG(warning) << "XTEST extension not available, falling back to uinput";
            XCloseDisplay(x_display);
            x_display = nullptr;
            use_xtest = false;
          }
        } else {
          BOOST_LOG(warning) << "Failed to open X11 display, falling back to uinput";
          use_xtest = false;
        }
      }

      if (!use_xtest)
#endif
      {
        if (!mouse) {
          BOOST_LOG(warning) << "Unable to create virtual mouse: " << mouse.getErrorMessage();
        }
        if (!keyboard) {
          BOOST_LOG(warning) << "Unable to create virtual keyboard: " << keyboard.getErrorMessage();
        }
      }
    }

    ~input_raw_t() {
#ifdef SUNSHINE_BUILD_X11
      if (x_display) {
        XCloseDisplay(x_display);
        x_display = nullptr;
      }
#endif
    }

#ifdef SUNSHINE_BUILD_X11
    // Flag to use XTEST instead of uinput (for Xvfb environments)
    bool use_xtest;

    // X11 display connection for XTEST (only used when use_xtest is true)
    Display *x_display;
#endif

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
