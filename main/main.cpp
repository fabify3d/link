/**
 * Fabify Link (of FabLink) based on ESP32S3 with USB VCP support, WiFi, 
 * HTTP, WebSocket, Command Processor, and Message Router for remotely controlling
 * CNC machines like 3D printers.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "string.h"
#include <cstdarg>
#include <cstdio>

#include "esp_log.h"
#include "esp_flash.h"

#include "include/espvserial.hpp"
#include "include/espserial.hpp"
#include "include/messagerouter.h"
#include "include/storage_manager.h"
#include "include/wifi_manager.h"
#include "include/webservermanager.h"
#include "include/commandprocessor.h"
#include "include/websocketclient.h"
#include "include/statusled.h"
#include "include/auth.h"

// using namespace esp_usb; // Removed because esp_usb is not a namespace
// for all logs, including in the header files and comonents, only show the errors and warnings
//#define LOG_LOCAL_LEVEL ESP_LOG_WARN


static const char *TAG = "FL";
static ESPVSerial vcp;

static constexpr const char* FABLINK_VERSION = "0.3.0";

// Define global instances
ESPSerial Serial(UART_NUM_0);  // UART0 (usually USB/debug)
//ESPSerial Serial1(UART_NUM_1); // UART1
//ESPSerial Serial2(UART_NUM_2); // UART2

Auth auth;
MessageRouter router;
WiFiManager wifi;
WebServerManager webServer;
CommandProcessor cmdProcessor;
WebSocketClient ws;
StatusLED status_led(48); // GPIO 48
static vprintf_like_t default_logger = nullptr;

extern "C" int custom_log_handler(const char *fmt, va_list args);

extern "C" void app_main(void)
{   
    esp_log_level_set("*", ESP_LOG_NONE); // Set default log level to WARN
    default_logger = esp_log_set_vprintf(custom_log_handler); // Save default

    status_led.setColorHex(FAB_ORANGE);

    // Initialize NVS first (required by WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "NVS initialized");

    auth.initBlocking();

    // Then initialize your LittleFS storage
    ret = storage_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize storage!");
        // return;
    }
    else
    {
        storage_set_string("device_name", "fablink");
    }

    Serial.begin(115200);
    Serial.println("Serial intialized");
    ws.begin();

    // Setup callbacks
    vcp.onConnect([]()
    {
        Serial.println("✓ Device connected!");
        Serial.printf("  Baud rate: %lu\n", vcp.getBaud());

        // Clear any stale data from connection
        vTaskDelay(pdMS_TO_TICKS(100));
        while (vcp.available() > 0) {
            vcp.read();
        }

        status_led.setColorHex(FAB_GREEN);
    });

    vcp.onDisconnect([]()
    {
        Serial.println("✗ Device disconnected!");
        status_led.setColorHex(FAB_ORANGE);
    });

    cmdProcessor.begin(&router);

    cmdProcessor.registerCommand("wifi", [](const ParsedCommand &parsed){
        if (parsed.args.empty()) {
            return "Error: Subcommand required\n";
        }
        std::string subcommand = parsed.args[0];

        if (subcommand == "connect") {
            if (parsed.args.size() < 3) {
                return "Usage: wifi connect <SSID> <PASSWORD>\n";
            }
            std::string ssid = parsed.args[1];
            std::string password = parsed.args[2];

            // Save to storage
            if(storage_set_string("wifi_ssid", ssid.c_str()) != ESP_OK) {
                return "Error: Failed to save SSID\n";
            }
            if(storage_set_string("wifi_password", password.c_str()) != ESP_OK) {
                return "Error: Failed to save Password\n";
            }

            // Connect to WiFi (async, returns immediately)
            esp_err_t res = wifi.connect(ssid.c_str(), password.c_str(), false);
            if(res != ESP_OK) {
                return "Error: Failed to initiate connection\n";
            }
            return "OK: WiFi connection initiated\n";
        } else if (subcommand == "disconnect") {
            wifi.disconnect();
            return "OK: Disconnecting from WiFi...\n";
        } else if (subcommand == "status") {
            return wifi.getStatus().c_str();
        } else {
            return "Error: Unknown subcommand\n";
        }

    }, "Manage WiFi\n\tUsage: wifi connect <SSID> <PASSWORD>\n\twifi disconnect\n\twifi status");

    cmdProcessor.registerCommand("ws", [](const ParsedCommand &parsed){
        if (parsed.args.empty()) {
            return "Error: Subcommand required\n";
        }
        std::string subcommand = parsed.args[0];

        if (subcommand == "connect") {
            if (parsed.args.size() < 2) {
                return "Usage: ws connect <URI>\n";
            }
            std::string uri = parsed.args[1];

            if (!uri.starts_with("ws://") && !uri.starts_with("wss://")) {
                return "Error: Invalid WebSocket URI\n";
            }

            // Save to storage
            if(storage_set_string("ws_uri", uri.c_str()) != ESP_OK) {
                return "Error: Failed to save URI\n";
            }

            // Connect to WebSocket (async, returns immediately)
            bool res = ws.connect(uri.c_str(), auth.getDeviceId().c_str());
            if(!res) {
                return "Error: Failed to initiate connection\n";
            }
            return "OK: WebSocket connection initiated\n";
        } else if (subcommand == "disconnect") {
            ws.disconnect();
            return "OK: Disconnecting from WebSocket...\n";
        } else if (subcommand == "status") {
            return ws.getStatus().c_str();
        } else {
            return "Error: Unknown subcommand\n";
        }

    }, "Manage WebSocket Client\n\tUsage: ws connect <URI>\n\tws disconnect\n\tws status");

    cmdProcessor.registerCommand( "auth", [](const ParsedCommand &parsed){
        if (parsed.args.empty()) {
            return auth.getDeviceId()+"\n";
        }

        std::string subcommand = parsed.args[0];

        if(subcommand == "pub-pem") {
            return auth.getPublicKeyPEM() + "\n";
        }

        return std::string("Invalid\n");
    }, "");

    cmdProcessor.registerCommand("status", [](const ParsedCommand &parsed)
    {
        std::string result;
        result.reserve(2048);  // Pre-allocate to avoid reallocations

        result += "\nSystem Status:\n";
        result += "FabLink: v" + std::string(FABLINK_VERSION) + "\n";
        result += "Free Heap: ";
        result += std::to_string(esp_get_free_heap_size() / 1024);
        result += " KB\n";
        
        result += "Min Free Heap: ";
        result += std::to_string(esp_get_minimum_free_heap_size() / 1024);
        result += " KB\n";
        
        result += "Uptime: ";
        result += std::to_string(esp_timer_get_time() / 1000000);
        result += " seconds\n";

        // Add each component's status separately to avoid temporary buildup
        result += "\nVCP:\n";
        result += vcp.getStatus();
        result += "\n";

        result += "WiFi:\n";
        result += wifi.getStatus();
        result += "\n";
        
        result += "WebServer:\n";
        result += webServer.getStatus();
        result += "\n";
        
        result += "WebSocket:\n";
        result += ws.getStatus();
        result += "\n";
        
        result += "Message Router:\n";
        result += router.getStatus();
        result += "\n";

        return result; 
    }, "Show system status", "status");
    // Messages for Serial (UART0) are sent to Serial
    router.addListener("/", [](const MessageRouter::Message &msg, const std::string &path, const std::string &senderID)
    { 
        if(senderID == "s0") {
            return; // Ignore messages from self
        }

        Serial.println(msg.data.c_str());
    }, "s0");

     // Initialize message router
    router.addListener("/com/s1", [](const MessageRouter::Message &msg, const std::string &path, const std::string &senderID)
    { 
        if(senderID == "s1") {
            return; // Ignore messages from self
        }

        vcp.printf(msg.data.c_str());
    }, "s1");

    router.addListener("/sys/cmd", [](const MessageRouter::Message &msg, const std::string &path, const std::string &senderID)
    { 
        if(senderID == "cmd") {
            return; // Ignore messages from self
        }

        std::string response = cmdProcessor.processCommand(msg.data);
        if(!response.empty()) {
            router.sendMessage("/com", response, "cmd");
        }
        response.clear(); // free up memory

    }, "cmd");

    Serial.onMessage([](const char* msg) {

        std::string str(msg);
        if (str.starts_with("cmd:")) {
            // Command for command processor
            std::string command = str.substr(4); // Remove "cmd:" prefix
            std::string result = cmdProcessor.processCommand(command);
            Serial.println(result.c_str());

            // free up memory
            result.clear();
            command.clear();
            str.clear();

            return;
        }

        router.sendMessage("/com/s1", str, "s0");
        str.clear(); // free up memory
    }, '\n');

    vcp.onMessage([](const char *msg)
    { 
        router.sendMessage("/com/s1", std::string(msg), "s1");

    },'\n'); // Messages end with newline

    ws.onMessage([](const char* msg, const char* path, const char* sender) {
        router.sendMessage(path, msg, "ws");
    });
    router.addListener("/", [](const MessageRouter::Message &msg, const std::string &path, const std::string &senderID)
    { 
        if(senderID == "ws") {
            return; // Ignore messages from self
        }

        ws.send(msg.data.c_str(), "/ws", "server");
    }, "ws");

    wifi.begin();
    wifi.startAP("Fabify-Link", "fablink123");

    // Start server on port 80
    webServer.start(80);
    webServer.setCommandProcessor(&cmdProcessor);

    // get wifi creds
    char ssid[64];
    char password[64];
    char uri[256];
    if(storage_get_string("wifi_ssid", ssid, sizeof(ssid)) != ESP_OK) {
        ESP_LOGI(TAG, "WiFi SSID not set");
        ssid[0] = '\0';
        password[0] = '\0';
    } else if(storage_get_string("wifi_password", password, sizeof(password)) != ESP_OK) {
        ESP_LOGI(TAG, "WiFi Password not set");
        password[0] = '\0';
    } else {
        ESP_LOGI(TAG, "WiFi SSID: %s", ssid);
        ESP_LOGI(TAG, "WiFi Password: %s", password);

        // connect to wifi
        wifi.connect(ssid, password);

        for (int i = 0; i < 20; i++) {
            if (wifi.isConnected()) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        if (storage_get_string("ws_uri", uri, sizeof(uri)) == ESP_OK && wifi.isConnected()) {
            ws.connect(uri, auth.getDeviceId().c_str());
        }
    }

    // Check if running
    if (webServer.running())
    {
        printf("Server is running\n");
    } else {
        printf("Server failed to start\n");
    }

    // Main loop - handle device connections
    while (true)
    {
        Serial.loop();

        if (!vcp.isConnected())
        {
            if (!vcp.begin())
            {
                ESP_LOGI(TAG, "Waiting for VCP device...");
                vTaskDelay(pdMS_TO_TICKS(200)); // delay reduce from 2000
                continue;
            }

            ESP_LOGI(TAG, "VCP device connected, configuring...");

            vcp.setBaud(115200);
            vcp.setLineCoding(8, 0, 0);      // 8 data bits, 1 stop bit, no parity
            vcp.setControlLines(true, true); // Assert DTR and RTS

            vTaskDelay(pdMS_TO_TICKS(200));

            // Send initial command to identify
            vcp.printf("\r\nM115\r\n");
            ESP_LOGI(TAG, "VCP device ready");

            continue;
        }

        vcp.loop();

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" int custom_log_handler(const char *fmt, va_list args)
{
    static bool in_handler = false;
    if (in_handler)
        return vprintf(fmt, args); // Prevent recursion

    in_handler = true;

    char msg[256];
    vsnprintf(msg, sizeof(msg), fmt, args);
    int ret = printf("%s", msg);

    if (strstr(msg, "E ("))
    {
        status_led.setColorHex(FAB_RED);
        status_led.setBlinkingMode(BlinkMode::FAST);
    }
    else if (strstr(msg, "W ("))
    {
        status_led.setColorHex(FAB_CORAL);
        status_led.setBlinkingMode(BlinkMode::NORMAL);
    }

    in_handler = false;
    return ret;
}
