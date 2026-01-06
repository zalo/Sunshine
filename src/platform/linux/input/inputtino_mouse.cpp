/**
 * @file src/platform/linux/input/inputtino_mouse.cpp
 * @brief Definitions for inputtino mouse input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>

// local includes
#include "inputtino_common.h"
#include "inputtino_mouse.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::mouse {

  // Screen dimensions for XTEST (hardcoded for now, could be made configurable)
  static constexpr int SCREEN_WIDTH = 1920;
  static constexpr int SCREEN_HEIGHT = 1080;

  void move(input_raw_t *raw, int deltaX, int deltaY) {
#ifdef SUNSHINE_BUILD_X11
    if (raw->use_xtest && raw->x_display) {
      // XTEST relative motion via XTestFakeRelativeMotionEvent
      XTestFakeRelativeMotionEvent(raw->x_display, deltaX, deltaY, CurrentTime);
      XFlush(raw->x_display);
    } else
#endif
    if (raw->mouse) {
      (*raw->mouse).move(deltaX, deltaY);
    }
  }

  void move_abs(input_raw_t *raw, const touch_port_t &touch_port, float x, float y) {
#ifdef SUNSHINE_BUILD_X11
    if (raw->use_xtest && raw->x_display) {
      // Convert from 0-65535 normalized coords to screen coords
      int screen_x = static_cast<int>(x * SCREEN_WIDTH / 65535.0f);
      int screen_y = static_cast<int>(y * SCREEN_HEIGHT / 65535.0f);
      XTestFakeMotionEvent(raw->x_display, DefaultScreen(raw->x_display), screen_x, screen_y, CurrentTime);
      XFlush(raw->x_display);
    } else
#endif
    if (raw->mouse) {
      (*raw->mouse).move_abs(x, y, touch_port.width, touch_port.height);
    }
  }

  void button(input_raw_t *raw, int button, bool release) {
#ifdef SUNSHINE_BUILD_X11
    if (raw->use_xtest && raw->x_display) {
      // X11 button mapping: 1=left, 2=middle, 3=right, 8=back, 9=forward
      unsigned int x_button;
      switch (button) {
        case BUTTON_LEFT:
          x_button = 1;
          break;
        case BUTTON_MIDDLE:
          x_button = 2;
          break;
        case BUTTON_RIGHT:
          x_button = 3;
          break;
        case BUTTON_X1:
          x_button = 8;  // Back
          break;
        case BUTTON_X2:
          x_button = 9;  // Forward
          break;
        default:
          BOOST_LOG(warning) << "Unknown mouse button: " << button;
          return;
      }
      XTestFakeButtonEvent(raw->x_display, x_button, !release, CurrentTime);
      XFlush(raw->x_display);
    } else
#endif
    if (raw->mouse) {
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
#ifdef SUNSHINE_BUILD_X11
    if (raw->use_xtest && raw->x_display) {
      // X11 button 4 = scroll up, button 5 = scroll down
      // high_res_distance is typically 120 units per "click"
      int clicks = high_res_distance / 120;
      unsigned int button = (clicks > 0) ? 4 : 5;
      int abs_clicks = (clicks > 0) ? clicks : -clicks;
      for (int i = 0; i < abs_clicks && i < 10; i++) {
        XTestFakeButtonEvent(raw->x_display, button, True, CurrentTime);
        XTestFakeButtonEvent(raw->x_display, button, False, CurrentTime);
      }
      XFlush(raw->x_display);
    } else
#endif
    if (raw->mouse) {
      (*raw->mouse).vertical_scroll(high_res_distance);
    }
  }

  void hscroll(input_raw_t *raw, int high_res_distance) {
#ifdef SUNSHINE_BUILD_X11
    if (raw->use_xtest && raw->x_display) {
      // X11 button 6 = scroll left, button 7 = scroll right
      int clicks = high_res_distance / 120;
      unsigned int button = (clicks > 0) ? 7 : 6;
      int abs_clicks = (clicks > 0) ? clicks : -clicks;
      for (int i = 0; i < abs_clicks && i < 10; i++) {
        XTestFakeButtonEvent(raw->x_display, button, True, CurrentTime);
        XTestFakeButtonEvent(raw->x_display, button, False, CurrentTime);
      }
      XFlush(raw->x_display);
    } else
#endif
    if (raw->mouse) {
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
