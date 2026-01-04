/**
 * @file src/webrtc/ws_server.h
 * @brief WebSocket server for WebRTC signaling
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace webrtc {

  /**
   * @brief WebSocket connection handle
   */
  using ws_connection_id = uint64_t;

  /**
   * @brief Callback for received messages
   */
  using message_callback_t = std::function<void(ws_connection_id, const std::string &)>;

  /**
   * @brief Callback for connection events
   */
  using connection_callback_t = std::function<void(ws_connection_id)>;

  /**
   * @brief WebSocket server for signaling
   */
  class WebSocketServer {
  public:
    WebSocketServer();
    ~WebSocketServer();

    /**
     * @brief Start the WebSocket server
     * @param port The port to listen on
     * @param use_ssl Whether to use SSL/TLS
     * @param cert_path Path to SSL certificate (if use_ssl)
     * @param key_path Path to SSL private key (if use_ssl)
     * @return True if started successfully
     */
    bool start(uint16_t port, bool use_ssl = false,
               const std::string &cert_path = "",
               const std::string &key_path = "");

    /**
     * @brief Stop the WebSocket server
     */
    void stop();

    /**
     * @brief Check if the server is running
     */
    bool is_running() const;

    /**
     * @brief Send a message to a specific connection
     * @param conn_id The connection ID
     * @param message The message to send
     * @return True if sent successfully
     */
    bool send(ws_connection_id conn_id, const std::string &message);

    /**
     * @brief Broadcast a message to all connections
     * @param message The message to send
     */
    void broadcast(const std::string &message);

    /**
     * @brief Close a connection
     * @param conn_id The connection ID
     */
    void close_connection(ws_connection_id conn_id);

    /**
     * @brief Set callback for received messages
     */
    void set_message_callback(message_callback_t callback);

    /**
     * @brief Set callback for new connections
     */
    void set_connect_callback(connection_callback_t callback);

    /**
     * @brief Set callback for disconnections
     */
    void set_disconnect_callback(connection_callback_t callback);

    /**
     * @brief Get the number of active connections
     */
    size_t connection_count() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
  };

  /**
   * @brief Global WebSocket server instance
   */
  WebSocketServer &ws_server();

}  // namespace webrtc
