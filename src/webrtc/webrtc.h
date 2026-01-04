/**
 * @file src/webrtc/webrtc.h
 * @brief WebRTC streaming module for multiplayer browser support.
 *
 * This module enables up to 4 players to connect via web browsers and
 * participate in local multiplayer games. Video/audio is fanned out to
 * all connected peers, and gamepad input is routed to appropriate slots.
 */
#pragma once

#include <memory>
#include <string>

namespace webrtc {

  /**
   * @brief Initialize the WebRTC module.
   * @return 0 on success, non-zero on failure.
   */
  int
  init();

  /**
   * @brief Start the WebRTC signaling server.
   * This integrates with the existing HTTPS server.
   */
  void
  start();

  /**
   * @brief Stop the WebRTC module and disconnect all peers.
   */
  void
  stop();

  /**
   * @brief Check if WebRTC streaming is enabled in config.
   * @return true if enabled, false otherwise.
   */
  bool
  is_enabled();

  /**
   * @brief Get the maximum number of players allowed.
   * @return Maximum player count (1-4).
   */
  int
  max_players();

  // Configuration keys for sunshine.conf
  namespace config {
    constexpr auto ENABLED = "webrtc_enabled";
    constexpr auto PORT_RANGE_MIN = "webrtc_port_range_min";
    constexpr auto PORT_RANGE_MAX = "webrtc_port_range_max";
    constexpr auto STUN_SERVER = "webrtc_stun_server";
    constexpr auto TURN_SERVER = "webrtc_turn_server";
    constexpr auto TURN_USERNAME = "webrtc_turn_username";
    constexpr auto TURN_PASSWORD = "webrtc_turn_password";
    constexpr auto MAX_PLAYERS = "webrtc_max_players";
  }  // namespace config

}  // namespace webrtc
