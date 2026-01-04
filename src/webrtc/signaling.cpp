/**
 * @file src/webrtc/signaling.cpp
 * @brief WebSocket signaling implementation.
 */

#include "signaling.h"

#include <nlohmann/json.hpp>

#include "peer.h"
#include "room.h"
#include "video_sender.h"
#include "webrtc.h"
#include "ws_server.h"

#include "src/config.h"
#include "src/logging.h"

using json = nlohmann::json;

namespace webrtc {

  SignalingServer &
  SignalingServer::instance() {
    static SignalingServer instance;
    return instance;
  }

  void
  SignalingServer::init() {
    // Set up WebSocket server callbacks
    ws_server().set_connect_callback([this](ws_connection_id conn_id) {
      on_ws_connect(conn_id);
    });

    ws_server().set_disconnect_callback([this](ws_connection_id conn_id) {
      on_ws_disconnect(conn_id);
    });

    ws_server().set_message_callback([this](ws_connection_id conn_id, const std::string &message) {
      on_ws_message(conn_id, message);
    });

    BOOST_LOG(info) << "WebRTC signaling server initialized";
  }

  // Port offset for WebSocket signaling (main port + PORT_WEBRTC_WS)
  constexpr int PORT_WEBRTC_WS = 2;  // After PORT_HTTPS (1)

  void
  SignalingServer::start() {
    running_.store(true);

    // Get WebSocket port using the same mapping as other Sunshine ports
    // This makes it ::config::sunshine.port + PORT_WEBRTC_WS
    uint16_t ws_port = static_cast<uint16_t>(::config::sunshine.port + PORT_WEBRTC_WS);

    // Use SSL with Sunshine's existing certificates
    bool use_ssl = true;
    std::string cert_path = ::config::nvhttp.cert;
    std::string key_path = ::config::nvhttp.pkey;

    if (!ws_server().start(ws_port, use_ssl, cert_path, key_path)) {
      // Try without SSL as fallback
      BOOST_LOG(warning) << "Failed to start WebSocket server with SSL, trying without SSL";
      if (!ws_server().start(ws_port, false, "", "")) {
        BOOST_LOG(error) << "Failed to start WebSocket signaling server";
        return;
      }
    }

    BOOST_LOG(info) << "WebRTC signaling server started on port " << ws_port;
  }

  void
  SignalingServer::stop() {
    running_.store(false);
    ws_server().stop();
    BOOST_LOG(info) << "WebRTC signaling server stopped";
  }

  void
  SignalingServer::on_ws_connect(ws_connection_id conn_id) {
    // Generate a peer ID for this connection
    std::string peer_id = "peer_" + std::to_string(conn_id);

    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      ws_to_peer_[conn_id] = peer_id;
      peer_to_ws_[peer_id] = conn_id;
    }

    on_open(peer_id);
  }

  void
  SignalingServer::on_ws_disconnect(ws_connection_id conn_id) {
    std::string peer_id;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      auto it = ws_to_peer_.find(conn_id);
      if (it == ws_to_peer_.end()) {
        return;
      }
      peer_id = it->second;
    }

    on_close(peer_id);
  }

  void
  SignalingServer::on_ws_message(ws_connection_id conn_id, const std::string &message) {
    std::string peer_id;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      auto it = ws_to_peer_.find(conn_id);
      if (it == ws_to_peer_.end()) {
        return;
      }
      peer_id = it->second;
    }

    on_message(peer_id, message);
  }

  void
  SignalingServer::on_open(const std::string &peer_id) {
    BOOST_LOG(info) << "WebRTC client connected: " << peer_id;
  }

  void
  SignalingServer::on_close(const std::string &peer_id) {
    BOOST_LOG(info) << "WebRTC client disconnected: " << peer_id;

    // Find and handle room cleanup
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (room) {
      bool host_left = room->remove_peer(peer_id);

      if (host_left) {
        // Notify all remaining peers that room is closing
        json msg;
        msg["type"] = "room_closed";
        msg["reason"] = "host_left";

        for (const auto &peer : room->get_peers()) {
          send_to_peer(peer->id(), msg.dump());
        }

        RoomManager::instance().remove_room(room->code());
      }
      else {
        // Notify remaining peers about player leaving
        auto player = room->get_player(peer_id);
        if (player) {
          json msg;
          msg["type"] = "player_left";
          msg["peer_id"] = peer_id;
          msg["slot"] = static_cast<int>(player->slot);

          broadcast_to_room(room->code(), msg.dump(), peer_id);

          // Send updated player list
          json update;
          update["type"] = "room_updated";
          update["players"] = json::parse(build_players_json(room->code()));

          broadcast_to_room(room->code(), update.dump());
        }
      }
    }

    // Clean up peer
    PeerManager::instance().remove_peer(peer_id);

    // Remove from connection maps
    std::lock_guard<std::mutex> lock(connections_mutex_);
    auto ws_it = peer_to_ws_.find(peer_id);
    if (ws_it != peer_to_ws_.end()) {
      ws_to_peer_.erase(ws_it->second);
      peer_to_ws_.erase(ws_it);
    }
  }

  void
  SignalingServer::on_message(const std::string &peer_id, const std::string &message) {
    try {
      auto msg = json::parse(message);
      std::string type = msg.value("type", "");

      BOOST_LOG(debug) << "WebRTC message from " << peer_id << ": " << type;

      if (type == "create_room") {
        handle_create_room(peer_id, msg.value("player_name", "Player"));
      }
      else if (type == "join_room") {
        handle_join_room(peer_id, msg.value("room_code", ""), msg.value("player_name", "Player"));
      }
      else if (type == "leave_room") {
        handle_leave_room(peer_id);
      }
      else if (type == "join_as_player") {
        handle_join_as_player(peer_id);
      }
      else if (type == "claim_gamepad") {
        handle_claim_gamepad(peer_id, msg.value("gamepad_id", 0));
      }
      else if (type == "release_gamepad") {
        handle_release_gamepad(peer_id, msg.value("server_slot", 0));
      }
      else if (type == "sdp") {
        handle_sdp(peer_id, msg.value("sdp", ""), msg.value("sdp_type", ""));
      }
      else if (type == "ice") {
        handle_ice(peer_id, msg.value("candidate", ""), msg.value("mid", ""));
      }
      else if (type == "set_guest_keyboard") {
        handle_set_guest_keyboard(peer_id, msg.value("peer_id", ""), msg.value("enabled", false));
      }
      else if (type == "set_guest_mouse") {
        handle_set_guest_mouse(peer_id, msg.value("peer_id", ""), msg.value("enabled", false));
      }
      else {
        send_error(peer_id, "Unknown message type: " + type, "unknown_type");
      }
    }
    catch (const json::exception &e) {
      BOOST_LOG(error) << "Failed to parse WebRTC message: " << e.what();
      send_error(peer_id, "Invalid JSON message", "parse_error");
    }
  }

  void
  SignalingServer::handle_create_room(const std::string &peer_id, const std::string &player_name) {
    // Create peer connection
    auto peer = PeerManager::instance().create_peer(peer_id);
    if (!peer) {
      send_error(peer_id, "Failed to create peer connection", "peer_error");
      return;
    }

    // Create room
    auto room = RoomManager::instance().create_room(peer, player_name);
    if (!room) {
      PeerManager::instance().remove_peer(peer_id);
      send_error(peer_id, "Failed to create room", "room_error");
      return;
    }

    // Set up peer callbacks for signaling
    peer->on_local_description([this, peer_id](const std::string &sdp, const std::string &type) {
      json msg;
      msg["type"] = "sdp";
      msg["sdp"] = sdp;
      msg["sdp_type"] = type;
      send_to_peer(peer_id, msg.dump());
    });

    peer->on_local_candidate([this, peer_id](const std::string &candidate, const std::string &mid) {
      json msg;
      msg["type"] = "ice";
      msg["candidate"] = candidate;
      msg["mid"] = mid;
      send_to_peer(peer_id, msg.dump());
    });

    peer->on_state_change([this, peer_id](PeerState state) {
      if (state == PeerState::CONNECTED) {
        json msg;
        msg["type"] = "stream_ready";
        send_to_peer(peer_id, msg.dump());
      }
    });

    // Determine video codec based on current encoder
    std::string codec = "H264";  // Default
    auto video_params = VideoSender::instance().get_params();
    switch (video_params.codec) {
      case VideoCodec::HEVC:
        codec = "HEVC";
        break;
      case VideoCodec::AV1:
        codec = "AV1";
        break;
      default:
        codec = "H264";
    }

    // Add media tracks
    peer->add_video_track(codec);
    peer->add_audio_track();

    // Create data channels for input
    peer->create_data_channel("input");

    // Send room created response
    json response;
    response["type"] = "room_created";
    response["room_code"] = room->code();
    response["player_slot"] = 1;
    response["is_host"] = true;

    send_to_peer(peer_id, response.dump());

    BOOST_LOG(info) << "Room " << room->code() << " created for " << player_name;
  }

  void
  SignalingServer::handle_join_room(const std::string &peer_id,
    const std::string &room_code,
    const std::string &player_name) {
    // Find room
    auto room = RoomManager::instance().find_room(room_code);
    if (!room) {
      send_error(peer_id, "Room not found", "room_not_found");
      return;
    }

    // Create peer connection
    auto peer = PeerManager::instance().create_peer(peer_id);
    if (!peer) {
      send_error(peer_id, "Failed to create peer connection", "peer_error");
      return;
    }

    // Add as spectator
    if (!room->add_spectator(peer, player_name)) {
      PeerManager::instance().remove_peer(peer_id);
      send_error(peer_id, "Failed to join room", "join_error");
      return;
    }

    // Set up peer callbacks
    peer->on_local_description([this, peer_id](const std::string &sdp, const std::string &type) {
      json msg;
      msg["type"] = "sdp";
      msg["sdp"] = sdp;
      msg["sdp_type"] = type;
      send_to_peer(peer_id, msg.dump());
    });

    peer->on_local_candidate([this, peer_id](const std::string &candidate, const std::string &mid) {
      json msg;
      msg["type"] = "ice";
      msg["candidate"] = candidate;
      msg["mid"] = mid;
      send_to_peer(peer_id, msg.dump());
    });

    peer->on_state_change([this, peer_id](PeerState state) {
      if (state == PeerState::CONNECTED) {
        json msg;
        msg["type"] = "stream_ready";
        send_to_peer(peer_id, msg.dump());
      }
    });

    // Add media tracks
    std::string codec = "H264";
    auto video_params = VideoSender::instance().get_params();
    switch (video_params.codec) {
      case VideoCodec::HEVC:
        codec = "HEVC";
        break;
      case VideoCodec::AV1:
        codec = "AV1";
        break;
      default:
        codec = "H264";
    }

    peer->add_video_track(codec);
    peer->add_audio_track();
    peer->create_data_channel("input");

    // Send join response
    json response;
    response["type"] = "room_joined";
    response["room_code"] = room_code;
    response["player_slot"] = 0;  // Spectator
    response["is_spectator"] = true;
    response["is_host"] = false;
    response["players"] = json::parse(build_players_json(room_code));

    send_to_peer(peer_id, response.dump());

    // Notify other players
    json join_msg;
    join_msg["type"] = "player_joined";
    join_msg["player"]["peer_id"] = peer_id;
    join_msg["player"]["name"] = player_name;
    join_msg["player"]["slot"] = 0;
    join_msg["player"]["is_spectator"] = true;

    broadcast_to_room(room_code, join_msg.dump(), peer_id);

    BOOST_LOG(info) << player_name << " joined room " << room_code << " as spectator";
  }

  void
  SignalingServer::handle_leave_room(const std::string &peer_id) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      send_error(peer_id, "Not in a room", "not_in_room");
      return;
    }

    std::string room_code = room->code();
    bool host_left = room->remove_peer(peer_id);

    if (host_left) {
      // Close room
      json msg;
      msg["type"] = "room_closed";
      msg["reason"] = "host_left";

      for (const auto &peer : room->get_peers()) {
        send_to_peer(peer->id(), msg.dump());
      }

      RoomManager::instance().remove_room(room_code);
    }
    else {
      // Notify others
      auto player = room->get_player(peer_id);
      if (player) {
        json msg;
        msg["type"] = "player_left";
        msg["peer_id"] = peer_id;
        msg["slot"] = static_cast<int>(player->slot);

        broadcast_to_room(room_code, msg.dump(), peer_id);
      }
    }

    json response;
    response["type"] = "left_room";
    send_to_peer(peer_id, response.dump());
  }

  void
  SignalingServer::handle_join_as_player(const std::string &peer_id) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      send_error(peer_id, "Not in a room", "not_in_room");
      return;
    }

    PlayerSlot slot = room->promote_to_player(peer_id);
    if (slot == PlayerSlot::NONE) {
      send_error(peer_id, "No player slots available", "room_full");
      return;
    }

    // Send promotion response
    json response;
    response["type"] = "promoted_to_player";
    response["player_slot"] = static_cast<int>(slot);

    send_to_peer(peer_id, response.dump());

    // Notify other players
    json update;
    update["type"] = "room_updated";
    update["players"] = json::parse(build_players_json(room->code()));

    broadcast_to_room(room->code(), update.dump());

    BOOST_LOG(info) << "Peer " << peer_id << " promoted to player slot " << static_cast<int>(slot);
  }

  void
  SignalingServer::handle_claim_gamepad(const std::string &peer_id, int gamepad_id) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      send_error(peer_id, "Not in a room", "not_in_room");
      return;
    }

    int server_slot = room->claim_gamepad(peer_id, gamepad_id);
    if (server_slot < 0) {
      send_error(peer_id, "Failed to claim gamepad", "gamepad_error");
      return;
    }

    json response;
    response["type"] = "gamepad_claimed";
    response["gamepad_id"] = gamepad_id;
    response["server_slot"] = server_slot;

    send_to_peer(peer_id, response.dump());
  }

  void
  SignalingServer::handle_release_gamepad(const std::string &peer_id, int server_slot) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      send_error(peer_id, "Not in a room", "not_in_room");
      return;
    }

    room->release_gamepad(peer_id, server_slot);

    json response;
    response["type"] = "gamepad_released";
    response["server_slot"] = server_slot;

    send_to_peer(peer_id, response.dump());
  }

  void
  SignalingServer::handle_sdp(const std::string &peer_id,
    const std::string &sdp,
    const std::string &type) {
    auto peer = PeerManager::instance().find_peer(peer_id);
    if (!peer) {
      send_error(peer_id, "Peer not found", "peer_not_found");
      return;
    }

    peer->set_remote_description(sdp, type);

    // If we received an offer, we need to create an answer
    if (type == "offer") {
      peer->create_description("answer");
    }
  }

  void
  SignalingServer::handle_ice(const std::string &peer_id,
    const std::string &candidate,
    const std::string &mid) {
    auto peer = PeerManager::instance().find_peer(peer_id);
    if (!peer) {
      send_error(peer_id, "Peer not found", "peer_not_found");
      return;
    }

    peer->add_ice_candidate(candidate, mid);
  }

  void
  SignalingServer::handle_set_guest_keyboard(const std::string &peer_id,
    const std::string &target_peer_id,
    bool enabled) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      send_error(peer_id, "Not in a room", "not_in_room");
      return;
    }

    // Only host can modify permissions
    if (!room->is_host(peer_id)) {
      send_error(peer_id, "Only host can modify permissions", "not_host");
      return;
    }

    if (room->set_keyboard_access(target_peer_id, enabled)) {
      // Notify the target peer
      json msg;
      msg["type"] = "permission_changed";
      msg["keyboard_enabled"] = enabled;
      send_to_peer(target_peer_id, msg.dump());

      // Send updated room state to all
      json update;
      update["type"] = "room_updated";
      update["players"] = json::parse(build_players_json(room->code()));
      broadcast_to_room(room->code(), update.dump());
    }
  }

  void
  SignalingServer::handle_set_guest_mouse(const std::string &peer_id,
    const std::string &target_peer_id,
    bool enabled) {
    auto room = RoomManager::instance().find_room_by_peer(peer_id);
    if (!room) {
      send_error(peer_id, "Not in a room", "not_in_room");
      return;
    }

    if (!room->is_host(peer_id)) {
      send_error(peer_id, "Only host can modify permissions", "not_host");
      return;
    }

    if (room->set_mouse_access(target_peer_id, enabled)) {
      json msg;
      msg["type"] = "permission_changed";
      msg["mouse_enabled"] = enabled;
      send_to_peer(target_peer_id, msg.dump());

      json update;
      update["type"] = "room_updated";
      update["players"] = json::parse(build_players_json(room->code()));
      broadcast_to_room(room->code(), update.dump());
    }
  }

  bool
  SignalingServer::send_to_peer(const std::string &peer_id, const std::string &message) {
    BOOST_LOG(debug) << "Send to peer " << peer_id << ": " << message.substr(0, 100);

    ws_connection_id conn_id;
    {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      auto it = peer_to_ws_.find(peer_id);
      if (it == peer_to_ws_.end()) {
        BOOST_LOG(warning) << "No WebSocket connection for peer " << peer_id;
        return false;
      }
      conn_id = it->second;
    }

    return ws_server().send(conn_id, message);
  }

  void
  SignalingServer::broadcast_to_room(const std::string &room_code,
    const std::string &message,
    const std::string &exclude_peer_id) {
    auto room = RoomManager::instance().find_room(room_code);
    if (!room) {
      return;
    }

    for (const auto &peer : room->get_peers()) {
      if (peer->id() != exclude_peer_id) {
        send_to_peer(peer->id(), message);
      }
    }
  }

  size_t
  SignalingServer::connection_count() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    return peer_to_ws_.size();
  }

  void
  SignalingServer::send_error(const std::string &peer_id,
    const std::string &message,
    const std::string &code) {
    json msg;
    msg["type"] = "error";
    msg["message"] = message;
    msg["code"] = code;

    send_to_peer(peer_id, msg.dump());
  }

  std::string
  SignalingServer::build_players_json(const std::string &room_code) {
    auto room = RoomManager::instance().find_room(room_code);
    if (!room) {
      return "[]";
    }

    json players = json::array();
    for (const auto &player : room->get_players()) {
      json p;
      p["peer_id"] = player.peer_id;
      p["name"] = player.name;
      p["slot"] = static_cast<int>(player.slot);
      p["is_host"] = player.is_host;
      p["is_spectator"] = player.is_spectator;
      p["can_use_keyboard"] = player.can_use_keyboard;
      p["can_use_mouse"] = player.can_use_mouse;
      p["gamepad_count"] = player.gamepad_ids.size();

      players.push_back(p);
    }

    return players.dump();
  }

}  // namespace webrtc
