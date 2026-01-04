/**
 * @file src/webrtc/ws_server.cpp
 * @brief WebSocket server implementation using libdatachannel
 */

#include "ws_server.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <rtc/rtc.hpp>

#include "src/logging.h"

namespace webrtc {

  /**
   * @brief WebSocket server implementation using libdatachannel
   */
  class WebSocketServerImpl {
  public:
    WebSocketServerImpl() :
        running_(false),
        next_conn_id_(1) {}

    ~WebSocketServerImpl() {
      stop();
    }

    bool start(uint16_t port, bool use_ssl,
               const std::string &cert_path,
               const std::string &key_path) {
      if (running_) {
        return false;
      }

      try {
        rtc::WebSocketServerConfiguration config;
        config.port = port;
        config.enableTls = use_ssl;

        if (use_ssl && !cert_path.empty() && !key_path.empty()) {
          config.certificatePemFile = cert_path;
          config.keyPemFile = key_path;
        }

        server_ = std::make_unique<rtc::WebSocketServer>(config);

        server_->onClient([this](std::shared_ptr<rtc::WebSocket> ws) {
          on_client(ws);
        });

        running_ = true;
        BOOST_LOG(info) << "WebSocket signaling server started on port " << port;
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Failed to start WebSocket server: " << e.what();
        return false;
      }
    }

    void stop() {
      if (!running_) {
        return;
      }

      running_ = false;

      // Close all connections
      {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (auto &[id, ws] : connections_) {
          try {
            ws->close();
          }
          catch (...) {}
        }
        connections_.clear();
      }

      server_.reset();
      BOOST_LOG(info) << "WebSocket signaling server stopped";
    }

    bool is_running() const {
      return running_;
    }

    bool send(ws_connection_id conn_id, const std::string &message) {
      std::lock_guard<std::mutex> lock(connections_mutex_);

      auto it = connections_.find(conn_id);
      if (it == connections_.end()) {
        return false;
      }

      try {
        it->second->send(message);
        return true;
      }
      catch (const std::exception &e) {
        BOOST_LOG(error) << "Failed to send message: " << e.what();
        return false;
      }
    }

    void broadcast(const std::string &message) {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      for (auto &[id, ws] : connections_) {
        try {
          ws->send(message);
        }
        catch (...) {}
      }
    }

    void close_connection(ws_connection_id conn_id) {
      std::lock_guard<std::mutex> lock(connections_mutex_);

      auto it = connections_.find(conn_id);
      if (it != connections_.end()) {
        try {
          it->second->close();
        }
        catch (...) {}
        connections_.erase(it);
      }
    }

    void set_message_callback(message_callback_t callback) {
      message_callback_ = std::move(callback);
    }

    void set_connect_callback(connection_callback_t callback) {
      connect_callback_ = std::move(callback);
    }

    void set_disconnect_callback(connection_callback_t callback) {
      disconnect_callback_ = std::move(callback);
    }

    size_t connection_count() const {
      std::lock_guard<std::mutex> lock(connections_mutex_);
      return connections_.size();
    }

  private:
    void on_client(std::shared_ptr<rtc::WebSocket> ws) {
      ws_connection_id conn_id = next_conn_id_++;

      {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connections_[conn_id] = ws;
      }

      ws->onOpen([this, conn_id]() {
        BOOST_LOG(debug) << "WebSocket connection " << conn_id << " opened";
        if (connect_callback_) {
          connect_callback_(conn_id);
        }
      });

      ws->onClosed([this, conn_id]() {
        BOOST_LOG(debug) << "WebSocket connection " << conn_id << " closed";
        {
          std::lock_guard<std::mutex> lock(connections_mutex_);
          connections_.erase(conn_id);
        }
        if (disconnect_callback_) {
          disconnect_callback_(conn_id);
        }
      });

      ws->onError([this, conn_id](const std::string &err_msg) {
        BOOST_LOG(error) << "WebSocket connection " << conn_id << " error: " << err_msg;
        {
          std::lock_guard<std::mutex> lock(connections_mutex_);
          connections_.erase(conn_id);
        }
        if (disconnect_callback_) {
          disconnect_callback_(conn_id);
        }
      });

      ws->onMessage([this, conn_id](rtc::message_variant data) {
        if (std::holds_alternative<std::string>(data)) {
          const auto &message = std::get<std::string>(data);
          if (message_callback_) {
            message_callback_(conn_id, message);
          }
        }
      });
    }

    std::unique_ptr<rtc::WebSocketServer> server_;
    std::atomic<bool> running_;
    std::atomic<ws_connection_id> next_conn_id_;

    mutable std::mutex connections_mutex_;
    std::unordered_map<ws_connection_id, std::shared_ptr<rtc::WebSocket>> connections_;

    message_callback_t message_callback_;
    connection_callback_t connect_callback_;
    connection_callback_t disconnect_callback_;
  };

  // WebSocketServer PIMPL wrapper

  class WebSocketServer::Impl {
  public:
    WebSocketServerImpl server;
  };

  WebSocketServer::WebSocketServer() :
      impl_(std::make_unique<Impl>()) {}

  WebSocketServer::~WebSocketServer() = default;

  bool WebSocketServer::start(uint16_t port, bool use_ssl,
                              const std::string &cert_path,
                              const std::string &key_path) {
    return impl_->server.start(port, use_ssl, cert_path, key_path);
  }

  void WebSocketServer::stop() {
    impl_->server.stop();
  }

  bool WebSocketServer::is_running() const {
    return impl_->server.is_running();
  }

  bool WebSocketServer::send(ws_connection_id conn_id, const std::string &message) {
    return impl_->server.send(conn_id, message);
  }

  void WebSocketServer::broadcast(const std::string &message) {
    impl_->server.broadcast(message);
  }

  void WebSocketServer::close_connection(ws_connection_id conn_id) {
    impl_->server.close_connection(conn_id);
  }

  void WebSocketServer::set_message_callback(message_callback_t callback) {
    impl_->server.set_message_callback(std::move(callback));
  }

  void WebSocketServer::set_connect_callback(connection_callback_t callback) {
    impl_->server.set_connect_callback(std::move(callback));
  }

  void WebSocketServer::set_disconnect_callback(connection_callback_t callback) {
    impl_->server.set_disconnect_callback(std::move(callback));
  }

  size_t WebSocketServer::connection_count() const {
    return impl_->server.connection_count();
  }

  // Global instance
  WebSocketServer &ws_server() {
    static WebSocketServer instance;
    return instance;
  }

}  // namespace webrtc
