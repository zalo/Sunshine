/**
 * @file src/platform/linux/input/inputtino_keyboard.cpp
 * @brief Definitions for inputtino keyboard input handling.
 */
// lib includes
#include <boost/locale.hpp>
#include <inputtino/input.hpp>
#include <libevdev/libevdev.h>
#include <cstdlib>

// local includes
#include "inputtino_common.h"
#include "inputtino_keyboard.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/utility.h"

using namespace std::literals;

namespace platf::keyboard {

#ifdef SUNSHINE_BUILD_X11
  // Map Windows virtual key codes to X11 keysyms
  static KeySym vk_to_keysym(uint16_t vk) {
    switch (vk) {
      case 0x08: return XK_BackSpace;
      case 0x09: return XK_Tab;
      case 0x0D: return XK_Return;
      case 0x10: return XK_Shift_L;
      case 0x11: return XK_Control_L;
      case 0x12: return XK_Alt_L;
      case 0x13: return XK_Pause;
      case 0x14: return XK_Caps_Lock;
      case 0x1B: return XK_Escape;
      case 0x20: return XK_space;
      case 0x21: return XK_Page_Up;
      case 0x22: return XK_Page_Down;
      case 0x23: return XK_End;
      case 0x24: return XK_Home;
      case 0x25: return XK_Left;
      case 0x26: return XK_Up;
      case 0x27: return XK_Right;
      case 0x28: return XK_Down;
      case 0x2C: return XK_Print;
      case 0x2D: return XK_Insert;
      case 0x2E: return XK_Delete;
      case 0x30: return XK_0;
      case 0x31: return XK_1;
      case 0x32: return XK_2;
      case 0x33: return XK_3;
      case 0x34: return XK_4;
      case 0x35: return XK_5;
      case 0x36: return XK_6;
      case 0x37: return XK_7;
      case 0x38: return XK_8;
      case 0x39: return XK_9;
      case 0x41: return XK_a;
      case 0x42: return XK_b;
      case 0x43: return XK_c;
      case 0x44: return XK_d;
      case 0x45: return XK_e;
      case 0x46: return XK_f;
      case 0x47: return XK_g;
      case 0x48: return XK_h;
      case 0x49: return XK_i;
      case 0x4A: return XK_j;
      case 0x4B: return XK_k;
      case 0x4C: return XK_l;
      case 0x4D: return XK_m;
      case 0x4E: return XK_n;
      case 0x4F: return XK_o;
      case 0x50: return XK_p;
      case 0x51: return XK_q;
      case 0x52: return XK_r;
      case 0x53: return XK_s;
      case 0x54: return XK_t;
      case 0x55: return XK_u;
      case 0x56: return XK_v;
      case 0x57: return XK_w;
      case 0x58: return XK_x;
      case 0x59: return XK_y;
      case 0x5A: return XK_z;
      case 0x5B: return XK_Super_L;
      case 0x5C: return XK_Super_R;
      case 0x60: return XK_KP_0;
      case 0x61: return XK_KP_1;
      case 0x62: return XK_KP_2;
      case 0x63: return XK_KP_3;
      case 0x64: return XK_KP_4;
      case 0x65: return XK_KP_5;
      case 0x66: return XK_KP_6;
      case 0x67: return XK_KP_7;
      case 0x68: return XK_KP_8;
      case 0x69: return XK_KP_9;
      case 0x6A: return XK_KP_Multiply;
      case 0x6B: return XK_KP_Add;
      case 0x6D: return XK_KP_Subtract;
      case 0x6E: return XK_KP_Decimal;
      case 0x6F: return XK_KP_Divide;
      case 0x70: return XK_F1;
      case 0x71: return XK_F2;
      case 0x72: return XK_F3;
      case 0x73: return XK_F4;
      case 0x74: return XK_F5;
      case 0x75: return XK_F6;
      case 0x76: return XK_F7;
      case 0x77: return XK_F8;
      case 0x78: return XK_F9;
      case 0x79: return XK_F10;
      case 0x7A: return XK_F11;
      case 0x7B: return XK_F12;
      case 0x90: return XK_Num_Lock;
      case 0x91: return XK_Scroll_Lock;
      case 0xA0: return XK_Shift_L;
      case 0xA1: return XK_Shift_R;
      case 0xA2: return XK_Control_L;
      case 0xA3: return XK_Control_R;
      case 0xA4: return XK_Alt_L;
      case 0xA5: return XK_Alt_R;
      case 0xBA: return XK_semicolon;
      case 0xBB: return XK_equal;
      case 0xBC: return XK_comma;
      case 0xBD: return XK_minus;
      case 0xBE: return XK_period;
      case 0xBF: return XK_slash;
      case 0xC0: return XK_grave;
      case 0xDB: return XK_bracketleft;
      case 0xDC: return XK_backslash;
      case 0xDD: return XK_bracketright;
      case 0xDE: return XK_apostrophe;
      default: return NoSymbol;
    }
  }
#endif

  /**
   * Takes an UTF-32 encoded string and returns a hex string representation of the bytes (uppercase)
   *
   * ex: ['👱'] = "1F471" // see UTF encoding at https://www.compart.com/en/unicode/U+1F471
   *
   * adapted from: https://stackoverflow.com/a/7639754
   */
  std::string to_hex(const std::basic_string<char32_t> &str) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (const auto &ch : str) {
      ss << static_cast<uint32_t>(ch);
    }

    std::string hex_unicode(ss.str());
    std::ranges::transform(hex_unicode, hex_unicode.begin(), ::toupper);
    return hex_unicode;
  }

  /**
   * A map of linux scan code -> Moonlight keyboard code
   */
  static const std::map<short, short> key_mappings = {
    {KEY_BACKSPACE, 0x08},
    {KEY_TAB, 0x09},
    {KEY_ENTER, 0x0D},
    {KEY_LEFTSHIFT, 0x10},
    {KEY_LEFTCTRL, 0x11},
    {KEY_CAPSLOCK, 0x14},
    {KEY_ESC, 0x1B},
    {KEY_SPACE, 0x20},
    {KEY_PAGEUP, 0x21},
    {KEY_PAGEDOWN, 0x22},
    {KEY_END, 0x23},
    {KEY_HOME, 0x24},
    {KEY_LEFT, 0x25},
    {KEY_UP, 0x26},
    {KEY_RIGHT, 0x27},
    {KEY_DOWN, 0x28},
    {KEY_SYSRQ, 0x2C},
    {KEY_INSERT, 0x2D},
    {KEY_DELETE, 0x2E},
    {KEY_0, 0x30},
    {KEY_1, 0x31},
    {KEY_2, 0x32},
    {KEY_3, 0x33},
    {KEY_4, 0x34},
    {KEY_5, 0x35},
    {KEY_6, 0x36},
    {KEY_7, 0x37},
    {KEY_8, 0x38},
    {KEY_9, 0x39},
    {KEY_A, 0x41},
    {KEY_B, 0x42},
    {KEY_C, 0x43},
    {KEY_D, 0x44},
    {KEY_E, 0x45},
    {KEY_F, 0x46},
    {KEY_G, 0x47},
    {KEY_H, 0x48},
    {KEY_I, 0x49},
    {KEY_J, 0x4A},
    {KEY_K, 0x4B},
    {KEY_L, 0x4C},
    {KEY_M, 0x4D},
    {KEY_N, 0x4E},
    {KEY_O, 0x4F},
    {KEY_P, 0x50},
    {KEY_Q, 0x51},
    {KEY_R, 0x52},
    {KEY_S, 0x53},
    {KEY_T, 0x54},
    {KEY_U, 0x55},
    {KEY_V, 0x56},
    {KEY_W, 0x57},
    {KEY_X, 0x58},
    {KEY_Y, 0x59},
    {KEY_Z, 0x5A},
    {KEY_LEFTMETA, 0x5B},
    {KEY_RIGHTMETA, 0x5C},
    {KEY_KP0, 0x60},
    {KEY_KP1, 0x61},
    {KEY_KP2, 0x62},
    {KEY_KP3, 0x63},
    {KEY_KP4, 0x64},
    {KEY_KP5, 0x65},
    {KEY_KP6, 0x66},
    {KEY_KP7, 0x67},
    {KEY_KP8, 0x68},
    {KEY_KP9, 0x69},
    {KEY_KPASTERISK, 0x6A},
    {KEY_KPPLUS, 0x6B},
    {KEY_KPMINUS, 0x6D},
    {KEY_KPDOT, 0x6E},
    {KEY_KPSLASH, 0x6F},
    {KEY_F1, 0x70},
    {KEY_F2, 0x71},
    {KEY_F3, 0x72},
    {KEY_F4, 0x73},
    {KEY_F5, 0x74},
    {KEY_F6, 0x75},
    {KEY_F7, 0x76},
    {KEY_F8, 0x77},
    {KEY_F9, 0x78},
    {KEY_F10, 0x79},
    {KEY_F11, 0x7A},
    {KEY_F12, 0x7B},
    {KEY_NUMLOCK, 0x90},
    {KEY_SCROLLLOCK, 0x91},
    {KEY_LEFTSHIFT, 0xA0},
    {KEY_RIGHTSHIFT, 0xA1},
    {KEY_LEFTCTRL, 0xA2},
    {KEY_RIGHTCTRL, 0xA3},
    {KEY_LEFTALT, 0xA4},
    {KEY_RIGHTALT, 0xA5},
    {KEY_SEMICOLON, 0xBA},
    {KEY_EQUAL, 0xBB},
    {KEY_COMMA, 0xBC},
    {KEY_MINUS, 0xBD},
    {KEY_DOT, 0xBE},
    {KEY_SLASH, 0xBF},
    {KEY_GRAVE, 0xC0},
    {KEY_LEFTBRACE, 0xDB},
    {KEY_BACKSLASH, 0xDC},
    {KEY_RIGHTBRACE, 0xDD},
    {KEY_APOSTROPHE, 0xDE},
    {KEY_102ND, 0xE2}
  };

  void update(input_raw_t *raw, uint16_t modcode, bool release, uint8_t flags) {
#ifdef SUNSHINE_BUILD_X11
    if (raw->use_xtest && raw->x_display) {
      KeySym keysym = vk_to_keysym(modcode);
      if (keysym != NoSymbol) {
        KeyCode keycode = XKeysymToKeycode(raw->x_display, keysym);
        if (keycode != 0) {
          XTestFakeKeyEvent(raw->x_display, keycode, !release, CurrentTime);
          XFlush(raw->x_display);
        } else {
          BOOST_LOG(warning) << "XTEST: no keycode for keysym: " << keysym;
        }
      } else {
        BOOST_LOG(warning) << "XTEST: unknown virtual key code: " << modcode;
      }
    } else
#endif
    if (raw->keyboard) {
      if (release) {
        (*raw->keyboard).release(modcode);
      } else {
        (*raw->keyboard).press(modcode);
      }
    }
  }

  void unicode(input_raw_t *raw, char *utf8, int size) {
#ifdef SUNSHINE_BUILD_X11
    if (raw->use_xtest && raw->x_display) {
      /* Reading input text as UTF-8 */
      auto utf8_str = boost::locale::conv::to_utf<wchar_t>(utf8, utf8 + size, "UTF-8");
      /* Converting to UTF-32 */
      auto utf32_str = boost::locale::conv::utf_to_utf<char32_t>(utf8_str);
      /* To HEX string */
      auto hex_unicode = to_hex(utf32_str);
      BOOST_LOG(debug) << "Unicode XTEST, typing U+"sv << hex_unicode;

      // Use Ctrl+Shift+U method via XTEST
      KeyCode ctrl = XKeysymToKeycode(raw->x_display, XK_Control_L);
      KeyCode shift = XKeysymToKeycode(raw->x_display, XK_Shift_L);
      KeyCode u_key = XKeysymToKeycode(raw->x_display, XK_u);

      /* pressing <CTRL> + <SHIFT> + U */
      XTestFakeKeyEvent(raw->x_display, ctrl, True, CurrentTime);
      XTestFakeKeyEvent(raw->x_display, shift, True, CurrentTime);
      XTestFakeKeyEvent(raw->x_display, u_key, True, CurrentTime);
      XTestFakeKeyEvent(raw->x_display, u_key, False, CurrentTime);

      /* input each HEX character */
      for (auto &ch : hex_unicode) {
        KeySym hex_keysym;
        if (ch >= '0' && ch <= '9') {
          hex_keysym = XK_0 + (ch - '0');
        } else if (ch >= 'A' && ch <= 'F') {
          hex_keysym = XK_a + (ch - 'A');
        } else if (ch >= 'a' && ch <= 'f') {
          hex_keysym = XK_a + (ch - 'a');
        } else {
          continue;
        }
        KeyCode hex_keycode = XKeysymToKeycode(raw->x_display, hex_keysym);
        if (hex_keycode != 0) {
          XTestFakeKeyEvent(raw->x_display, hex_keycode, True, CurrentTime);
          XTestFakeKeyEvent(raw->x_display, hex_keycode, False, CurrentTime);
        }
      }

      /* releasing <SHIFT> and <CTRL> */
      XTestFakeKeyEvent(raw->x_display, shift, False, CurrentTime);
      XTestFakeKeyEvent(raw->x_display, ctrl, False, CurrentTime);
      XFlush(raw->x_display);
    } else
#endif
    if (raw->keyboard) {
      /* Reading input text as UTF-8 */
      auto utf8_str = boost::locale::conv::to_utf<wchar_t>(utf8, utf8 + size, "UTF-8");
      /* Converting to UTF-32 */
      auto utf32_str = boost::locale::conv::utf_to_utf<char32_t>(utf8_str);
      /* To HEX string */
      auto hex_unicode = to_hex(utf32_str);
      BOOST_LOG(debug) << "Unicode, typing U+"sv << hex_unicode;

      /* pressing <CTRL> + <SHIFT> + U */
      (*raw->keyboard).press(0xA2);  // LEFTCTRL
      (*raw->keyboard).press(0xA0);  // LEFTSHIFT
      (*raw->keyboard).press(0x55);  // U
      (*raw->keyboard).release(0x55);  // U

      /* input each HEX character */
      for (auto &ch : hex_unicode) {
        auto key_str = "KEY_"s + ch;
        auto keycode = libevdev_event_code_from_name(EV_KEY, key_str.c_str());
        auto wincode = key_mappings.find(keycode);
        if (keycode == -1 || wincode == key_mappings.end()) {
          BOOST_LOG(warning) << "Unicode, unable to find keycode for: "sv << ch;
        } else {
          (*raw->keyboard).press(wincode->second);
          (*raw->keyboard).release(wincode->second);
        }
      }

      /* releasing <SHIFT> and <CTRL> */
      (*raw->keyboard).release(0xA0);  // LEFTSHIFT
      (*raw->keyboard).release(0xA2);  // LEFTCTRL
    }
  }
}  // namespace platf::keyboard
