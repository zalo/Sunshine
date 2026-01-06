/**
 * @file src/platform/linux/input/inputtino_mouse.cpp
 * @brief Definitions for inputtino mouse input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>
#include <cstdlib>
#include <array>

// local includes
#include "inputtino_common.h"
#include "inputtino_mouse.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::mouse {

  // xdotool helper function
  static void xdotool_cmd(const std::vector<std::string> &args) {
    std::string cmd = "xdotool";
    for (const auto &arg : args) {
      cmd += " " + arg;
    }
    cmd += " 2>/dev/null";
    std::system(cmd.c_str());
  }

  // Screen dimensions for xdotool (hardcoded for now, could be made configurable)
  static constexpr int SCREEN_WIDTH = 1920;
  static constexpr int SCREEN_HEIGHT = 1080;

  void move(input_raw_t *raw, int deltaX, int deltaY) {
    if (raw->use_xdotool) {
      xdotool_cmd({"mousemove_relative", "--", std::to_string(deltaX), std::to_string(deltaY)});
    } else if (raw->mouse) {
      (*raw->mouse).move(deltaX, deltaY);
    }
  }

  void move_abs(input_raw_t *raw, const touch_port_t &touch_port, float x, float y) {
    if (raw->use_xdotool) {
      // Convert from 0-65535 normalized coords to screen coords
      int screen_x = static_cast<int>(x * SCREEN_WIDTH / 65535.0f);
      int screen_y = static_cast<int>(y * SCREEN_HEIGHT / 65535.0f);
      xdotool_cmd({"mousemove", std::to_string(screen_x), std::to_string(screen_y)});
    } else if (raw->mouse) {
      (*raw->mouse).move_abs(x, y, touch_port.width, touch_port.height);
    }
  }

  void button(input_raw_t *raw, int button, bool release) {
    if (raw->use_xdotool) {
      // xdotool button mapping: 1=left, 2=middle, 3=right, 8=back, 9=forward
      int xdotool_button;
      switch (button) {
        case BUTTON_LEFT:
          xdotool_button = 1;
          break;
        case BUTTON_MIDDLE:
          xdotool_button = 2;
          break;
        case BUTTON_RIGHT:
          xdotool_button = 3;
          break;
        case BUTTON_X1:
          xdotool_button = 8;  // Back
          break;
        case BUTTON_X2:
          xdotool_button = 9;  // Forward
          break;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: " << button;
          return;
      }
      if (release) {
        xdotool_cmd({"mouseup", std::to_string(xdotool_button)});
      } else {
        xdotool_cmd({"mousedown", std::to_string(xdotool_button)});
      }
    } else if (raw->mouse) {
      inputtino::Mouse::MOUSE_BUTTON btn_type;
      switch (button) {
        case BUTTON_LEFT:
          btn_type = inputtino::Mouse::LEFT;
          break;
        case BUTTON_MIDDLE:
          btn_type = inputtino::Mouse::MIDDLE;
          break;
        case BUTTON_RIGHT:
          btn_type = inputtino::Mouse::RIGHT;
          break;
        case BUTTON_X1:
          btn_type = inputtino::Mouse::SIDE;
          break;
        case BUTTON_X2:
          btn_type = inputtino::Mouse::EXTRA;
          break;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: " << button;
          return;
      }
      if (release) {
        (*raw->mouse).release(btn_type);
      } else {
        (*raw->mouse).press(btn_type);
      }
    }
  }

  void scroll(input_raw_t *raw, int high_res_distance) {
    if (raw->use_xdotool) {
      // xdotool uses button 4 (scroll up) and 5 (scroll down)
      // high_res_distance is typically 120 units per "click"
      int clicks = high_res_distance / 120;
      if (clicks > 0) {
        for (int i = 0; i < clicks && i < 10; i++) {
          xdotool_cmd({"click", "4"});
        }
      } else if (clicks < 0) {
        for (int i = 0; i > clicks && i > -10; i--) {
          xdotool_cmd({"click", "5"});
        }
      }
    } else if (raw->mouse) {
      (*raw->mouse).vertical_scroll(high_res_distance);
    }
  }

  void hscroll(input_raw_t *raw, int high_res_distance) {
    if (raw->use_xdotool) {
      // xdotool uses button 6 (scroll left) and 7 (scroll right)
      int clicks = high_res_distance / 120;
      if (clicks > 0) {
        for (int i = 0; i < clicks && i < 10; i++) {
          xdotool_cmd({"click", "7"});
        }
      } else if (clicks < 0) {
        for (int i = 0; i > clicks && i > -10; i--) {
          xdotool_cmd({"click", "6"});
        }
      }
    } else if (raw->mouse) {
      (*raw->mouse).horizontal_scroll(high_res_distance);
    }
  }

  util::point_t get_location(input_raw_t *raw) {
    if (raw->mouse) {
      // TODO: decide what to do after https://github.com/games-on-whales/inputtino/issues/6 is resolved.
      // TODO: auto x = (*raw->mouse).get_absolute_x();
      // TODO: auto y = (*raw->mouse).get_absolute_y();
      return {0, 0};
    }
    return {0, 0};
  }
}  // namespace platf::mouse
