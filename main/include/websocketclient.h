#ifndef WEBSOCKET_CLIENT_MANAGER_H
#define WEBSOCKET_CLIENT_MANAGER_H

#include "esp_websocket_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include <string>
#include <functional>
#include <queue>
#include <memory>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * WebSocket Client Manager for ESP-IDF
 *
 * Thread-safe WebSocket client with auto-reconnect and JSON messaging.
 * Handles connection lifecycle, reconnection, and message queuing automatically.
 *
 * Usage:
 *   WebSocketClient ws;
 *   ws.begin();
 *   ws.onMessage([](const char* msg, const char* path, const char* sender) {
 *       ESP_LOGI("WS", "Got: %s", msg);
 *   });
 *   ws.connect("ws://example.com", "my-client-id");
 */
class WebSocketClient
{
public:
    // Callback types
    using MessageCallback = std::function<void(const char *message, const char *path, const char *senderID)>;
    using ConnectionCallback = std::function<void()>;
    using ErrorCallback = std::function<void(const char *error)>;

    // Connection states
    enum class Status
    {
        DISCONNECTED = 0,
        CONNECTING = 1,
        HANDSHAKING = 2,
        CONNECTED = 3,
        RECONNECTING = 4,
        ERROR = 5
    };

    /**
     * Constructor with sensible defaults
     * @param autoReconnect Enable automatic reconnection (default: true)
     * @param maxQueueSize Maximum queued messages (default: 10)
     * @param reconnectDelay Delay between reconnects in ms (default: 5000)
     */
    WebSocketClient(bool autoReconnect = true,
                    size_t maxQueueSize = 50,
                    uint32_t reconnectDelay = 5000)
        : _autoReconnect(autoReconnect)
        , _maxQueueSize(maxQueueSize)
        , _reconnectDelay(reconnectDelay)
        , _tag("WebSocketClient")
        , _queueDropCount(0)
        , _client(nullptr)
        , _status(Status::DISCONNECTED)
        , _handshakeComplete(false)
        , _shouldReconnect(false)
        , _mutex(nullptr)
        , _reconnectTimer(nullptr)
    {
    }

    ~WebSocketClient()
    {
        disconnect();
        if (_mutex)
        {
            vSemaphoreDelete(_mutex);
        }
    }

    /**
     * Initialize the WebSocket client (call once at startup)
     * Sets up internal resources
     */
    bool begin()
    {
        if (_mutex != nullptr)
        {
            ESP_LOGW(_tag, "Already initialized");
            return true;
        }

        _mutex = xSemaphoreCreateRecursiveMutex();
        if (!_mutex)
        {
            ESP_LOGE(_tag, "Failed to create mutex");
            return false;
        }

        // Create reconnection timer
        esp_timer_create_args_t timer_args = {};
        timer_args.callback = _reconnectTimerCallback;
        timer_args.arg = this;
        timer_args.name = "ws_reconnect";

        esp_err_t err = esp_timer_create(&timer_args, &_reconnectTimer);
        if (err != ESP_OK)
        {
            ESP_LOGE(_tag, "Failed to create reconnect timer: %s", esp_err_to_name(err));
            vSemaphoreDelete(_mutex);
            _mutex = nullptr;
            return false;
        }

        ESP_LOGI(_tag, "WebSocket client initialized");
        return true;
    }

    /**
     * Connect to WebSocket server and perform handshake
     * @param url WebSocket URL (ws:// or wss://)
     * @param clientID Your unique client identifier
     * @return true if connection initiated successfully
     */
    bool connect(const char *url, const char *clientID)
    {
        if (!_mutex)
        {
            ESP_LOGE(_tag, "Not initialized - call begin() first");
            return false;
        }

        if (!url || !clientID || strlen(url) == 0 || strlen(clientID) == 0)
        {
            ESP_LOGE(_tag, "Invalid URL or client ID");
            return false;
        }

        // Validate URL format
        if (strncmp(url, "ws://", 5) != 0 && strncmp(url, "wss://", 6) != 0)
        {
            ESP_LOGE(_tag, "Invalid WebSocket URL - must start with ws:// or wss://");
            return false;
        }

        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

        Status currentStatus = _status.load();
        if (currentStatus == Status::CONNECTED || currentStatus == Status::CONNECTING)
        {
            ESP_LOGW(_tag, "Already connected or connecting, disconnecting first...");
            xSemaphoreGiveRecursive(_mutex);
            disconnect();
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        }

        _clientID = clientID;
        _url = url;
        _handshakeComplete = false;
        _shouldReconnect = true;

        xSemaphoreGiveRecursive(_mutex);

        return _connect(url);
    }

    /**
     * Disconnect from server
     * Stops auto-reconnect and clears message queue
     */
    void disconnect()
    {
        if (!_mutex)
            return;

        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

        _shouldReconnect = false;

        // Stop reconnect timer
        if (_reconnectTimer)
        {
            esp_timer_stop(_reconnectTimer);
        }

        if (_client)
        {
            _status.store(Status::DISCONNECTED);

            // Unregister events first to prevent callbacks during destruction
            esp_websocket_unregister_events(_client, WEBSOCKET_EVENT_ANY, _websocketEventHandler);

            esp_websocket_client_stop(_client);
            esp_websocket_client_destroy(_client);
            _client = nullptr;
            _handshakeComplete = false;

            // Clear message queue
            while (!_messageQueue.empty())
            {
                _messageQueue.pop();
            }

            ESP_LOGI(_tag, "Disconnected");
        }

        xSemaphoreGiveRecursive(_mutex);
    }

    /**
     * Send a message to the server
     * @param message Message content
     * @param path Message path/route (default: "/")
     * @param receiverID Message receiver (default: "server")
     * @return true if sent or queued successfully
     */
    bool send(const char *message, const char *path = "/", const char *receiverID = "server")
    {
        if (!_mutex)
        {
            ESP_LOGE(_tag, "Not initialized");
            return false;
        }

        if (!message || !path || !receiverID)
        {
            ESP_LOGE(_tag, "Invalid parameters");
            return false;
        }

        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

        if (!_handshakeComplete)
        {
            ESP_LOGW(_tag, "Handshake not complete, queueing message");
            bool result = _queueMessage(message, path, receiverID);
            xSemaphoreGiveRecursive(_mutex);
            return result;
        }

        bool result = _sendMessage(message, path, receiverID);
        xSemaphoreGiveRecursive(_mutex);
        return result;
    }

    /**
     * Set callback for incoming messages
     */
    void onMessage(MessageCallback callback)
    {
        if (_mutex)
        {
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
            _onMessage = callback;
            xSemaphoreGiveRecursive(_mutex);
        }
    }

    /**
     * Set callback for successful connection
     */
    void onConnected(ConnectionCallback callback)
    {
        if (_mutex)
        {
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
            _onConnected = callback;
            xSemaphoreGiveRecursive(_mutex);
        }
    }

    /**
     * Set callback for errors
     */
    void onError(ErrorCallback callback)
    {
        if (_mutex)
        {
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
            _onError = callback;
            xSemaphoreGiveRecursive(_mutex);
        }
    }

    /**
     * Check if connected and handshake complete
     */
    bool isOnline() const
    {
        return _status.load() == Status::CONNECTED && _handshakeComplete;
    }

    /**
     * Get current connection status
     */
    Status status() const
    {
        return _status.load();
    }

    /**
     * Get status as human-readable string
     */
    const char *statusString() const
    {
        switch (_status.load())
        {
        case Status::DISCONNECTED:
            return "Disconnected";
        case Status::CONNECTING:
            return "Connecting";
        case Status::HANDSHAKING:
            return "Handshaking";
        case Status::CONNECTED:
            return "Connected";
        case Status::RECONNECTING:
            return "Reconnecting";
        case Status::ERROR:
            return "Error";
        default:
            return "Unknown";
        }
    }

    /**
     * Set the default receiver ID for messages
     */
    void setReceiver(const char *receiverID)
    {
        if (_mutex && receiverID)
        {
            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
            _receiverID = receiverID;
            xSemaphoreGiveRecursive(_mutex);
        }
    }

    /**
     * Get the current receiver ID
     */
    std::string getReceiver() const
    {
        if (!_mutex)
            return "";

        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        std::string receiver = _receiverID;
        xSemaphoreGiveRecursive(_mutex);
        return receiver;
    }

    /**
     * Get detailed status information
     */
    std::string getStatus() const
    {
        if (!_mutex)
            return "Not initialized";

        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

        std::string result = "Status: " + std::string(statusString()) + "\n" +
                             "Client ID: " + _clientID + "\n" +
                             "Receiver ID: " + (_receiverID.empty() ? "N/A" : _receiverID) + "\n" +
                             "Auto-Reconnect: " + std::string(_autoReconnect ? "Yes" : "No") + "\n" +
                             "Queued Messages: " + std::to_string(_messageQueue.size()) + "/" +
                             std::to_string(_maxQueueSize) + "\n" +
                             "Queue dropped: " + std::to_string(_queueDropCount) + "\n";

        xSemaphoreGiveRecursive(_mutex);
        return result;
    }

private:
    // RAII wrapper for queued messages
    struct QueuedMessage
    {
        std::string message;
        std::string path;
        std::string receiverID;

        QueuedMessage(const char *msg, const char *p, const char *recv)
            : message(msg), path(p), receiverID(recv) {}
    };

    // Configuration
    bool _autoReconnect;
    size_t _maxQueueSize;
    uint32_t _reconnectDelay;
    const char *_tag;
    uint32_t _queueDropCount;

    // Connection state (thread-safe)
    esp_websocket_client_handle_t _client;
    std::atomic<Status> _status;
    std::string _clientID;
    std::string _url;
    std::string _receiverID;
    std::atomic<bool> _handshakeComplete;
    bool _shouldReconnect;

    // Synchronization
    SemaphoreHandle_t _mutex;
    esp_timer_handle_t _reconnectTimer;

    // Message queue (protected by mutex)
    std::queue<std::shared_ptr<QueuedMessage>> _messageQueue;

    // Callbacks (protected by mutex)
    MessageCallback _onMessage;
    ConnectionCallback _onConnected;
    ErrorCallback _onError;

    /**
     * Internal connection logic
     */
    bool _connect(const char *url)
    {
        _status.store(Status::CONNECTING);
        ESP_LOGI(_tag, "Connecting to %s as %s", url, _clientID.c_str());

        esp_websocket_client_config_t ws_cfg = {};
        ws_cfg.uri = url; // Will be copied by esp_websocket_client_init
        ws_cfg.reconnect_timeout_ms = _reconnectDelay;
        ws_cfg.network_timeout_ms = 10000;
        ws_cfg.buffer_size = 8192;
        ws_cfg.task_stack = 8192;
        ws_cfg.disable_auto_reconnect = true; // We handle reconnection ourselves

        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        _client = esp_websocket_client_init(&ws_cfg);
        xSemaphoreGiveRecursive(_mutex);

        if (!_client)
        {
            ESP_LOGE(_tag, "Failed to initialize WebSocket client");
            _status.store(Status::ERROR);
            _triggerErrorCallback("Failed to initialize client");
            _scheduleReconnect();
            return false;
        }

        esp_websocket_register_events(_client, WEBSOCKET_EVENT_ANY,
                                      _websocketEventHandler, this);

        esp_err_t err = esp_websocket_client_start(_client);
        if (err != ESP_OK)
        {
            ESP_LOGE(_tag, "Failed to start WebSocket client: %s", esp_err_to_name(err));
            _status.store(Status::ERROR);
            _triggerErrorCallback("Failed to start client");

            xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
            esp_websocket_client_destroy(_client);
            _client = nullptr;
            xSemaphoreGiveRecursive(_mutex);

            _scheduleReconnect();
            return false;
        }

        return true;
    }

    /**
     * Schedule reconnection using timer
     */
    void _scheduleReconnect()
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

        if (!_autoReconnect || !_shouldReconnect || !_reconnectTimer)
        {
            xSemaphoreGiveRecursive(_mutex);
            return;
        }

        _status.store(Status::RECONNECTING);
        ESP_LOGI(_tag, "Scheduling reconnect in %d ms", _reconnectDelay);

        esp_timer_stop(_reconnectTimer);                               // Stop any existing timer
        esp_timer_start_once(_reconnectTimer, _reconnectDelay * 1000); // Convert to microseconds

        xSemaphoreGiveRecursive(_mutex);
    }

    /**
     * Reconnect timer callback
     */
    static void _reconnectTimerCallback(void *arg)
    {
        WebSocketClient *self = (WebSocketClient *)arg;
        self->_attemptReconnect();
    }

    /**
     * Attempt to reconnect
     */
    void _attemptReconnect()
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        if (!_shouldReconnect || _url.empty()) {
            xSemaphoreGiveRecursive(_mutex);
            return;
        }
        if (_client) {
            esp_websocket_unregister_events(_client, WEBSOCKET_EVENT_ANY, _websocketEventHandler);
            esp_websocket_client_stop(_client);
            esp_websocket_client_destroy(_client);
            _client = nullptr;
        }
        std::string url = _url;  // copy before releasing mutex
        xSemaphoreGiveRecursive(_mutex);
        
        ESP_LOGI(_tag, "Attempting reconnection to %s", url.c_str());
        _connect(url.c_str());
    }

    /**
     * Perform handshake with server
     */
    void _performHandshake()
    {
        _status.store(Status::HANDSHAKING);
        ESP_LOGI(_tag, "Performing handshake");

        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        bool success = _sendMessage("", "", "server");
        xSemaphoreGiveRecursive(_mutex);

        if (!success)
        {
            ESP_LOGE(_tag, "Handshake failed to send");
            _status.store(Status::ERROR);
            _triggerErrorCallback("Handshake failed");
            _scheduleReconnect();
        }
    }

    /**
     * Handle handshake response
     */
    void _handleHandshakeResponse(const char *data, int len)
    {
        cJSON *json = cJSON_ParseWithLength(data, len);
        if (!json)
        {
            ESP_LOGE(_tag, "Handshake response parse failed");
            _status.store(Status::ERROR);
            _triggerErrorCallback("Invalid handshake response");
            _scheduleReconnect();
            return;
        }

        cJSON *msg = cJSON_GetObjectItem(json, "message");
        bool success = msg && msg->valuestring && strcmp(msg->valuestring, "ok") == 0;

        if (success)
        {
            _handshakeComplete.store(true);
            _status.store(Status::CONNECTED);
            ESP_LOGI(_tag, "Handshake successful - Connected!");

            _flushMessageQueue();
            _triggerConnectedCallback();
        }
        else
        {
            ESP_LOGE(_tag, "Handshake rejected by server");
            _status.store(Status::ERROR);
            _triggerErrorCallback("Handshake rejected");
            _scheduleReconnect();
        }

        cJSON_Delete(json);
    }

    /**
     * Send a JSON formatted message (must be called with mutex held)
     */
    bool _sendMessage(const char *message, const char *path, const char *receiverID)
    {
        if (!_client)
            return false;

        cJSON *json = cJSON_CreateObject();
        if (!json)
            return false;

        cJSON_AddStringToObject(json, "message", message ? message : "");
        cJSON_AddStringToObject(json, "path", path ? path : "");
        cJSON_AddStringToObject(json, "senderID", _clientID.c_str());
        cJSON_AddStringToObject(json, "receiverID", receiverID ? receiverID : "server");

        char *jsonStr = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);

        if (!jsonStr)
        {
            ESP_LOGE(_tag, "Failed to create JSON");
            return false;
        }

        int len = strlen(jsonStr);

        // Check message size
        if (len > 8192)
        {
            ESP_LOGE(_tag, "Message too large: %d bytes (max 8192)", len);
            free(jsonStr);
            return false;
        }

        int sent = esp_websocket_client_send_text(_client, jsonStr, len, pdMS_TO_TICKS(5000));
        free(jsonStr);

        if (sent < 0)
        {
            ESP_LOGE(_tag, "Failed to send message");
            _triggerErrorCallback("Send failed");
            return false;
        }

        ESP_LOGD(_tag, "Sent %d bytes", sent);
        return true;
    }

    /**
     * Queue message for later delivery (must be called with mutex held)
     */
    bool _queueMessage(const char *message, const char *path, const char *receiverID)
    {
        if (_messageQueue.size() >= _maxQueueSize)
        {
            ESP_LOGW(_tag, "Message queue full, dropping oldest");
            _messageQueue.pop();
            _queueDropCount++;
        }

        auto queued = std::make_shared<QueuedMessage>(message, path, receiverID);
        _messageQueue.push(queued);

        ESP_LOGD(_tag, "Message queued (%d in queue)", _messageQueue.size());
        return true;
    }

    /**
     * Send all queued messages
     */
    void _flushMessageQueue()
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);

        ESP_LOGI(_tag, "Flushing %d queued messages", _messageQueue.size());

        while (!_messageQueue.empty())
        {
            auto msg = _messageQueue.front();
            _messageQueue.pop();

            _sendMessage(msg->message.c_str(), msg->path.c_str(), msg->receiverID.c_str());
        }

        xSemaphoreGiveRecursive(_mutex);
    }

    /**
     * Trigger callbacks safely
     */
    void _triggerErrorCallback(const char *error)
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        ErrorCallback cb = _onError;
        xSemaphoreGiveRecursive(_mutex);
        if (cb)
        {
            cb(error);
        }
    }

    void _triggerConnectedCallback()
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        ConnectionCallback cb = _onConnected;
        xSemaphoreGiveRecursive(_mutex);
        if (cb)
        {
            cb();
        }
    }

    void _triggerMessageCallback(const char *message, const char *path, const char *sender)
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        MessageCallback cb = _onMessage;
        xSemaphoreGiveRecursive(_mutex);
        if (cb)
        {
            cb(message, path, sender);
        }
    }

    /**
     * WebSocket event handler
     */
    static void _websocketEventHandler(void *handler_args, esp_event_base_t base,
                                       int32_t event_id, void *event_data)
    {
        WebSocketClient *self = (WebSocketClient *)handler_args;
        esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

        switch (event_id)
        {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(self->_tag, "WebSocket connected");
            self->_performHandshake();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED: {
            ESP_LOGI(self->_tag, "WebSocket disconnected");
            self->_handshakeComplete.store(false);

            xSemaphoreTakeRecursive(self->_mutex, portMAX_DELAY);
            bool shouldReconnect = self->_shouldReconnect;
            xSemaphoreGiveRecursive(self->_mutex);

            if (self->_autoReconnect && shouldReconnect)
            {
                self->_scheduleReconnect();
            }
            else
            {
                self->_status.store(Status::DISCONNECTED);
            }
            }
            break;

        case WEBSOCKET_EVENT_DATA:
            // Handle close frame from server
            // Some ESP-IDF versions may not define WEBSOCKET_OPCODE_CLOSE; use the RFC6455 CLOSE opcode (0x8).
            if (data->op_code == 0x8) {
                ESP_LOGW(self->_tag, "Received a CLOSE frame from the server. Closing connection.");
                esp_websocket_client_close(self->_client, pdMS_TO_TICKS(1000));
                break;
            }

            if (data->data_len > 0 && data->data_ptr)
            {
                if (!self->_handshakeComplete.load())
                {
                    self->_handleHandshakeResponse((const char *)data->data_ptr, data->data_len);
                }
                else
                {
                    // Regular message - parse and validate
                    cJSON *json = cJSON_ParseWithLength((const char *)data->data_ptr, data->data_len);
                    if (json)
                    {
                        cJSON *msg = cJSON_GetObjectItem(json, "message");
                        cJSON *path = cJSON_GetObjectItem(json, "path");
                        cJSON *sender = cJSON_GetObjectItem(json, "senderID");
                        cJSON *receiver = cJSON_GetObjectItem(json, "receiverID");

                        // Validate all fields exist and have valid strings
                        if (receiver && receiver->valuestring &&
                            msg && path && sender &&
                            strcmp(receiver->valuestring, self->_clientID.c_str()) == 0)
                        {
                            const char *msgStr = msg->valuestring ? msg->valuestring : "";
                            const char *pathStr = path->valuestring ? path->valuestring : "/";
                            const char *senderStr = sender->valuestring ? sender->valuestring : "unknown";

                            self->_triggerMessageCallback(msgStr, pathStr, senderStr);
                        }
                        else
                        {
                            ESP_LOGD(self->_tag, "Ignoring message - receiverID mismatch or invalid fields");
                        }

                        cJSON_Delete(json);
                    }
                    else
                    {
                        ESP_LOGW(self->_tag, "Received non-JSON data");
                    }
                }
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(self->_tag, "WebSocket error");
            self->_status.store(Status::ERROR);
            self->_triggerErrorCallback("Connection error");
            self->_scheduleReconnect();
            break;

        default:
            break;
        }
    }
};

#endif // WEBSOCKET_CLIENT_MANAGER_H