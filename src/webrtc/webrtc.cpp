/**
 * @file src/webrtc/webrtc.cpp
 * @brief WebRTC module initialization and configuration.
 */

#include "webrtc.h"

#include <random>

#include <rtc/rtc.hpp>

#include "audio_sender.h"
#include "input.h"
#include "peer.h"
#include "room.h"
#include "signaling.h"
#include "video_sender.h"

#include "src/config.h"
#include "src/logging.h"

namespace webrtc {

  namespace {
    bool initialized = false;
    bool enabled = false;
    int configured_max_players = 4;
  }  // namespace

  int
  init() {
    if (initialized) {
      BOOST_LOG(warning) << "WebRTC module already initialized";
      return 0;
    }

    BOOST_LOG(info) << "Initializing WebRTC module";

    // Read configuration (use global config namespace)
    auto &config_vars = ::config::sunshine;

    // Check if WebRTC is enabled
    enabled = config_vars.flags[::config::flag::WEBRTC_ENABLED];
    if (!enabled) {
      BOOST_LOG(info) << "WebRTC streaming disabled in configuration";
      initialized = true;
      return 0;
    }

    // Get configuration values
    configured_max_players = std::clamp(config_vars.webrtc.max_players, 1, 4);

    // Configure STUN/TURN servers
    rtc::Configuration rtc_config;

    // Add STUN server
    if (!config_vars.webrtc.stun_server.empty()) {
      rtc_config.iceServers.emplace_back(config_vars.webrtc.stun_server);
      BOOST_LOG(info) << "WebRTC: Using STUN server: " << config_vars.webrtc.stun_server;
    }
    else {
      // Default to Google's public STUN server
      rtc_config.iceServers.emplace_back("stun:stun.l.google.com:19302");
      BOOST_LOG(info) << "WebRTC: Using default STUN server: stun:stun.l.google.com:19302";
    }

    // Add TURN server if configured
    if (!config_vars.webrtc.turn_server.empty()) {
      std::string turn_url = config_vars.webrtc.turn_server;

      // If username/password provided, add credentials
      if (!config_vars.webrtc.turn_username.empty()) {
        // Parse the TURN URL to extract hostname and port
        // Expected format: turn:hostname:port or turns:hostname:port
        std::string hostname;
        std::string port_str = "3478";  // Default TURN port
        rtc::IceServer::RelayType relay_type = rtc::IceServer::RelayType::TurnUdp;

        // Check for turns: (TLS)
        if (turn_url.find("turns:") == 0) {
          relay_type = rtc::IceServer::RelayType::TurnTls;
          hostname = turn_url.substr(6);  // Skip "turns:"
          port_str = "5349";  // Default TURNS port
        }
        else if (turn_url.find("turn:") == 0) {
          hostname = turn_url.substr(5);  // Skip "turn:"
        }
        else {
          hostname = turn_url;
        }

        // Extract port if present (hostname:port format)
        auto colon_pos = hostname.find(':');
        if (colon_pos != std::string::npos) {
          port_str = hostname.substr(colon_pos + 1);
          hostname = hostname.substr(0, colon_pos);
        }

        rtc::IceServer turn_server(
          hostname,
          port_str,
          config_vars.webrtc.turn_username,
          config_vars.webrtc.turn_password,
          relay_type
        );
        rtc_config.iceServers.push_back(turn_server);
        BOOST_LOG(info) << "WebRTC: Using TURN server: " << hostname << ":" << port_str << " with credentials";
      }
      else {
        rtc_config.iceServers.emplace_back(turn_url);
        BOOST_LOG(info) << "WebRTC: Using TURN server: " << turn_url;
      }
    }

    // Configure port range
    if (config_vars.webrtc.port_range_min > 0 && config_vars.webrtc.port_range_max > 0) {
      rtc_config.portRangeBegin = config_vars.webrtc.port_range_min;
      rtc_config.portRangeEnd = config_vars.webrtc.port_range_max;
      BOOST_LOG(info) << "WebRTC: Port range " << config_vars.webrtc.port_range_min
                      << "-" << config_vars.webrtc.port_range_max;
    }

    // Set the configuration in PeerManager
    PeerManager::instance().set_rtc_config(rtc_config);

    // Initialize libdatachannel
    rtc::InitLogger(rtc::LogLevel::Warning);

    // Initialize sub-modules
    SignalingServer::instance().init();
    InputHandler::instance().init();
    VideoSender::instance().init();
    AudioSender::instance().init();

    initialized = true;
    BOOST_LOG(info) << "WebRTC module initialized (max " << configured_max_players << " players)";

    return 0;
  }

  void
  start() {
    if (!initialized || !enabled) {
      return;
    }

    BOOST_LOG(info) << "Starting WebRTC streaming";

    SignalingServer::instance().start();
    VideoSender::instance().start();
    AudioSender::instance().start();
  }

  void
  stop() {
    if (!initialized || !enabled) {
      return;
    }

    BOOST_LOG(info) << "Stopping WebRTC streaming";

    VideoSender::instance().stop();
    AudioSender::instance().stop();
    SignalingServer::instance().stop();

    // Close all peer connections
    for (auto &peer : PeerManager::instance().get_peers()) {
      peer->close();
    }
  }

  bool
  is_enabled() {
    return enabled;
  }

  int
  max_players() {
    return configured_max_players;
  }

}  // namespace webrtc
