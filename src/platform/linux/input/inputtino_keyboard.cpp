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

  // xdotool helper function
  static void xdotool_cmd(const std::vector<std::string> &args) {
    std::string cmd = "xdotool";
    for (const auto &arg : args) {
      cmd += " " + arg;
    }
    cmd += " 2>/dev/null";
    std::system(cmd.c_str());
  }

  // Map Windows virtual key codes to xdotool key names
  static std::string vk_to_xdotool(uint16_t vk) {
    switch (vk) {
      case 0x08: return "BackSpace";
      case 0x09: return "Tab";
      case 0x0D: return "Return";
      case 0x10: return "Shift_L";
      case 0x11: return "Control_L";
      case 0x12: return "Alt_L";
      case 0x13: return "Pause";
      case 0x14: return "Caps_Lock";
      case 0x1B: return "Escape";
      case 0x20: return "space";
      case 0x21: return "Page_Up";
      case 0x22: return "Page_Down";
      case 0x23: return "End";
      case 0x24: return "Home";
      case 0x25: return "Left";
      case 0x26: return "Up";
      case 0x27: return "Right";
      case 0x28: return "Down";
      case 0x2C: return "Print";
      case 0x2D: return "Insert";
      case 0x2E: return "Delete";
      case 0x30: return "0";
      case 0x31: return "1";
      case 0x32: return "2";
      case 0x33: return "3";
      case 0x34: return "4";
      case 0x35: return "5";
      case 0x36: return "6";
      case 0x37: return "7";
      case 0x38: return "8";
      case 0x39: return "9";
      case 0x41: return "a";
      case 0x42: return "b";
      case 0x43: return "c";
      case 0x44: return "d";
      case 0x45: return "e";
      case 0x46: return "f";
      case 0x47: return "g";
      case 0x48: return "h";
      case 0x49: return "i";
      case 0x4A: return "j";
      case 0x4B: return "k";
      case 0x4C: return "l";
      case 0x4D: return "m";
      case 0x4E: return "n";
      case 0x4F: return "o";
      case 0x50: return "p";
      case 0x51: return "q";
      case 0x52: return "r";
      case 0x53: return "s";
      case 0x54: return "t";
      case 0x55: return "u";
      case 0x56: return "v";
      case 0x57: return "w";
      case 0x58: return "x";
      case 0x59: return "y";
      case 0x5A: return "z";
      case 0x5B: return "Super_L";
      case 0x5C: return "Super_R";
      case 0x60: return "KP_0";
      case 0x61: return "KP_1";
      case 0x62: return "KP_2";
      case 0x63: return "KP_3";
      case 0x64: return "KP_4";
      case 0x65: return "KP_5";
      case 0x66: return "KP_6";
      case 0x67: return "KP_7";
      case 0x68: return "KP_8";
      case 0x69: return "KP_9";
      case 0x6A: return "KP_Multiply";
      case 0x6B: return "KP_Add";
      case 0x6D: return "KP_Subtract";
      case 0x6E: return "KP_Decimal";
      case 0x6F: return "KP_Divide";
      case 0x70: return "F1";
      case 0x71: return "F2";
      case 0x72: return "F3";
      case 0x73: return "F4";
      case 0x74: return "F5";
      case 0x75: return "F6";
      case 0x76: return "F7";
      case 0x77: return "F8";
      case 0x78: return "F9";
      case 0x79: return "F10";
      case 0x7A: return "F11";
      case 0x7B: return "F12";
      case 0x90: return "Num_Lock";
      case 0x91: return "Scroll_Lock";
      case 0xA0: return "Shift_L";
      case 0xA1: return "Shift_R";
      case 0xA2: return "Control_L";
      case 0xA3: return "Control_R";
      case 0xA4: return "Alt_L";
      case 0xA5: return "Alt_R";
      case 0xBA: return "semicolon";
      case 0xBB: return "equal";
      case 0xBC: return "comma";
      case 0xBD: return "minus";
      case 0xBE: return "period";
      case 0xBF: return "slash";
      case 0xC0: return "grave";
      case 0xDB: return "bracketleft";
      case 0xDC: return "backslash";
      case 0xDD: return "bracketright";
      case 0xDE: return "apostrophe";
      default: return "";
    }
  }

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
    if (raw->use_xdotool) {
      std::string key = vk_to_xdotool(modcode);
      if (!key.empty()) {
        if (release) {
          xdotool_cmd({"keyup", key});
        } else {
          xdotool_cmd({"keydown", key});
        }
      } else {
        BOOST_LOG(warning) << "xdotool: unknown key code: " << modcode;
      }
    } else if (raw->keyboard) {
      if (release) {
        (*raw->keyboard).release(modcode);
      } else {
        (*raw->keyboard).press(modcode);
      }
    }
  }

  void unicode(input_raw_t *raw, char *utf8, int size) {
    if (raw->use_xdotool) {
      // xdotool can type UTF-8 directly
      std::string text(utf8, size);
      // Escape special characters for shell
      std::string escaped;
      for (char c : text) {
        if (c == '\'' || c == '\\') {
          escaped += '\\';
        }
        escaped += c;
      }
      xdotool_cmd({"type", "--", "'" + escaped + "'"});
    } else if (raw->keyboard) {
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
