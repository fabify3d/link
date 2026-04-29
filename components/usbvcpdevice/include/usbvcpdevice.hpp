/**
 * @file USBVCPDevice.hpp
 * @brief Abstract class for USB Virtual COM Port devices with auto-detection
 *
 * Simplifies development with USB VCP devices by providing a clean interface
 * that wraps ESP-IDF CDC-ACM host functionality with automatic driver registration.
 */

//#pragma once

#include <functional>
#include <memory>
#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb/vcp.hpp"
#include "usb/vcp_ch34x.hpp"
#include "usb/vcp_cp210x.hpp"
#include "usb/vcp_ftdi.hpp"

using namespace esp_usb;

/**
 * @brief Abstract base class for USB VCP devices with auto-detection
 *
 * Provides a simple, Arduino-like interface for working with USB VCP devices.
 * Handles USB host initialization, automatic driver registration, device
 * connection/disconnection, and data I/O.
 *
 * Supports the following VCP chipsets out of the box:
 * - FTDI (FT232, FT2232, FT4232, FT230X, FT231X, FT234XD)
 * - Silicon Labs CP210x (CP2102, CP2103, CP2104, CP2105, CP2108)
 * - WCH CH34x (CH340, CH341)
 */
class USBVCPDevice
{
public:
    /**
     * @brief Constructor
     */
    USBVCPDevice()
        : vcp_device_(nullptr), rx_queue_(nullptr), device_connected_(false), disconnect_sem_(nullptr), usb_task_handle_(nullptr), connect_cb_(nullptr), disconnect_cb_(nullptr), rx_cb_(nullptr), drivers_registered_(false)
    {
    }

    /**
     * @brief Destructor
     */
    virtual ~USBVCPDevice()
    {
        end();
    }

    /**
     * @brief Initialize and open VCP device with auto-detection
     *
     * Automatically registers common VCP drivers (FTDI, CP210x, CH34x) and
     * opens any compatible device that's connected.
     *
     * @param vendor_id Optional vendor ID to filter device (0 = any)
     * @param product_id Optional product ID to filter device (0 = any)
     * @param timeout_ms Connection timeout in milliseconds
     * @return true if device opened successfully
     */
    bool begin(uint16_t vendor_id = 0, uint16_t product_id = 0, uint32_t timeout_ms = 5000)
    {
        if (device_connected_)
        {
            ESP_LOGW(TAG, "Device already connected");
            return true;
        }

        // If we have a stale device handle, clean it up first
        if (vcp_device_)
        {
            ESP_LOGW(TAG, "Cleaning up previous device instance");
            vcp_device_.reset();
            vTaskDelay(pdMS_TO_TICKS(100)); // Give USB stack time to clean up
        }

        // Create synchronization primitives
        if (!disconnect_sem_)
        {
            disconnect_sem_ = xSemaphoreCreateBinary();
            if (!disconnect_sem_)
            {
                ESP_LOGE(TAG, "Failed to create disconnect semaphore");
                return false;
            }
        }

        // Create ring buffer for received data (4KB)
        if (!rx_queue_)
        {
            rx_queue_ = xQueueCreate(4096, sizeof(uint8_t));
            if (!rx_queue_)
            {
                ESP_LOGE(TAG, "Failed to create RX queue");
                if (disconnect_sem_)
                {
                    vSemaphoreDelete(disconnect_sem_);
                    disconnect_sem_ = nullptr;
                }
                return false;
            }
        }
        else
        {
            // Clear any stale data from previous connection
            xQueueReset(rx_queue_);
        }

        // Install USB Host driver (only once)
        if (!usb_host_installed_)
        {
            ESP_LOGI(TAG, "Installing USB Host");
            usb_host_config_t host_config = {};
            host_config.skip_phy_setup = false;
            host_config.intr_flags = ESP_INTR_FLAG_LOWMED;

            esp_err_t err = usb_host_install(&host_config);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "USB Host install failed: %s", esp_err_to_name(err));
                cleanup();
                return false;
            }
            usb_host_installed_ = true;

            // Create USB library handling task
            BaseType_t task_created = xTaskCreate(usb_lib_task_wrapper, "usb_lib", 4096, this, 10, &usb_task_handle_);
            if (task_created != pdTRUE)
            {
                ESP_LOGE(TAG, "Failed to create USB task");
                cleanup();
                return false;
            }
        }

        // Install CDC-ACM driver
        if (!cdc_acm_installed_)
        {
            ESP_LOGI(TAG, "Installing CDC-ACM driver");
            esp_err_t err = cdc_acm_host_install(NULL);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "CDC-ACM install failed: %s", esp_err_to_name(err));
                cleanup();
                return false;
            }
            cdc_acm_installed_ = true;
        }

        // Register VCP drivers for auto-detection (only once)
        if (!drivers_registered_)
        {
            ESP_LOGI(TAG, "Registering VCP drivers for auto-detection");
            registerVCPDrivers();
            drivers_registered_ = true;
        }

        // Open VCP device
        const cdc_acm_host_device_config_t dev_config = {
            .connection_timeout_ms = timeout_ms,
            .out_buffer_size = 512,
            .in_buffer_size = 512,
            .event_cb = event_callback_wrapper,
            .data_cb = data_callback_wrapper,
            .user_arg = this,
        };

        if (vendor_id != 0 && product_id != 0)
        {
            ESP_LOGI(TAG, "Opening VCP device (VID:0x%04X PID:0x%04X)...", vendor_id, product_id);
            vcp_device_.reset(VCP::open(vendor_id, product_id, &dev_config));
        }
        else
        {
            ESP_LOGI(TAG, "Opening any VCP device (auto-detection)...");
            vcp_device_.reset(VCP::open(&dev_config));
        }

        if (!vcp_device_)
        {
            ESP_LOGE(TAG, "Failed to open VCP device");
            return false;
        }

        device_connected_ = true;
        ESP_LOGI(TAG, "VCP device opened successfully");

        // Trigger connect callback
        if (connect_cb_)
        {
            connect_cb_();
        }

        return true;
    }

    /**
     * @brief Close VCP device and cleanup
     */
    void end()
    {
        if (vcp_device_)
        {
            ESP_LOGI(TAG, "Closing VCP device...");
            vcp_device_.reset();
        }
        device_connected_ = false;

        cleanup();
    }

    /**
     * @brief Check if device is connected
     */
    bool isConnected() const
    {
        return device_connected_;
    }

    /**
     * @brief Get number of bytes available for reading
     */
    int available()
    {
        if (!rx_queue_)
            return 0;
        return uxQueueMessagesWaiting(rx_queue_);
    }

    /**
     * @brief Read data from device
     *
     * @param buf Buffer to store read data
     * @param len Maximum number of bytes to read
     * @return Number of bytes actually read
     */
    size_t read(uint8_t *buf, size_t len)
    {
        if (!buf || len == 0 || !rx_queue_)
        {
            return 0;
        }

        size_t bytes_read = 0;
        while (bytes_read < len && xQueueReceive(rx_queue_, &buf[bytes_read], 0) == pdTRUE)
        {
            bytes_read++;
        }

        return bytes_read;
    }

    /**
     * @brief Read a single byte (blocking with timeout)
     *
     * @param timeout_ms Timeout in milliseconds
     * @return Byte read or -1 on timeout/error
     */
    int read(uint32_t timeout_ms = 0)
    {
        if (!rx_queue_)
            return -1;

        uint8_t byte;
        TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);

        if (xQueueReceive(rx_queue_, &byte, ticks) == pdTRUE)
        {
            return byte;
        }

        return -1;
    }

    /**
     * @brief Peek at next byte without removing it
     *
     * @return Next byte or -1 if no data available
     */
    int peek()
    {
        if (!rx_queue_)
            return -1;

        uint8_t byte;
        if (xQueuePeek(rx_queue_, &byte, 0) == pdTRUE)
        {
            return byte;
        }

        return -1;
    }

    /**
     * @brief Write data to device
     *
     * @param buf Data to write
     * @param len Number of bytes to write
     * @return Number of bytes written
     */
    size_t write(const uint8_t *buf, size_t len)
    {
        if (!device_connected_ || !vcp_device_ || !buf || len == 0)
        {
            return 0;
        }

        // tx_blocking expects non-const pointer, so we need to cast
        // (the data is not modified by the function despite the signature)
        esp_err_t err = vcp_device_->tx_blocking(const_cast<uint8_t *>(buf), len);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Write failed: %s", esp_err_to_name(err));
            return 0;
        }

        return len;
    }

    /**
     * @brief Write a single byte
     *
     * @param byte Byte to write
     * @return 1 if successful, 0 otherwise
     */
    size_t write(uint8_t byte)
    {
        return write(&byte, 1);
    }

    /**
     * @brief Write a null-terminated string
     *
     * @param str String to write
     * @return Number of bytes written
     */
    size_t write(const char *str)
    {
        if (!str)
            return 0;
        return write((const uint8_t *)str, strlen(str));
    }

    /**
     * @brief Print formatted string
     *
     * @param format Printf-style format string
     * @param ... Variable arguments
     * @return Number of bytes written
     */
    size_t printf(const char *format, ...)
    {
        char buffer[256];
        va_list args;
        va_start(args, format);
        int len = vsnprintf(buffer, sizeof(buffer), format, args);
        va_end(args);

        if (len > 0)
        {
            return write((const uint8_t *)buffer, len);
        }
        return 0;
    }

    /**
     * @brief Flush output buffer (no-op for blocking writes)
     */
    void flush()
    {
        // Blocking writes mean data is already flushed
    }

    /**
     * @brief Set baud rate
     *
     * @param baud Desired baud rate
     * @return true if successful
     */
    bool setBaud(uint32_t baud)
    {
        if (!device_connected_ || !vcp_device_)
        {
            return false;
        }

        cdc_acm_line_coding_t line_coding;
        esp_err_t err = vcp_device_->line_coding_get(&line_coding);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to get line coding");
            // set default code and return
            return false;
        }

        line_coding.dwDTERate = baud;
        err = vcp_device_->line_coding_set(&line_coding);

        return (err == ESP_OK);
    }

    /**
     * @brief Get current baud rate
     *
     * @return Current baud rate or 0 on error
     */
    uint32_t getBaud()
    {
        if (!device_connected_ || !vcp_device_)
        {
            return 0;
        }

        cdc_acm_line_coding_t line_coding;
        esp_err_t err = vcp_device_->line_coding_get(&line_coding);
        if (err != ESP_OK)
        {
            return 0;
        }

        return line_coding.dwDTERate;
    }

    /**
     * @brief Set line coding parameters
     *
     * @param data_bits Number of data bits (5-8)
     * @param stop_bits Stop bits (0=1, 1=1.5, 2=2)
     * @param parity Parity (0=None, 1=Odd, 2=Even, 3=Mark, 4=Space)
     * @return true if successful
     */
    bool setLineCoding(uint8_t data_bits, uint8_t stop_bits, uint8_t parity)
    {
        if (!device_connected_ || !vcp_device_)
        {
            return false;
        }

        cdc_acm_line_coding_t line_coding;
        esp_err_t err = vcp_device_->line_coding_get(&line_coding);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to get line coding");
            return false;
        }

        line_coding.bDataBits = data_bits;
        line_coding.bCharFormat = stop_bits;
        line_coding.bParityType = parity;

        err = vcp_device_->line_coding_set(&line_coding);
        return (err == ESP_OK);
    }

    /**
     * @brief Get DTR and RTS line states
     *
     * @param dtr Reference to store DTR state
     * @param rts Reference to store RTS state
     * @return true if successful
     */
    bool getLineState(bool &dtr, bool &rts)
    {
        // Note: CDC-ACM host typically sets these, not reads them
        // This would need additional driver support
        ESP_LOGW(TAG, "getLineState not fully supported by CDC-ACM host");
        return false;
    }

    /**
     * @brief Set DTR and RTS control lines
     *
     * @param dtr DTR state
     * @param rts RTS state
     * @return true if successful
     */
    bool setControlLines(bool dtr, bool rts)
    {
        if (!device_connected_ || !vcp_device_)
        {
            return false;
        }

        esp_err_t err = vcp_device_->set_control_line_state(dtr, rts);
        return (err == ESP_OK);
    }

    /**
     * @brief Register connect callback
     */
    void onConnect(std::function<void()> cb)
    {
        connect_cb_ = cb;
    }

    /**
     * @brief Register disconnect callback
     */
    void onDisconnect(std::function<void()> cb)
    {
        disconnect_cb_ = cb;
    }

    /**
     * @brief Register RX data callback
     *
     * @param cb Callback function receiving number of bytes available
     */
    void onRX(std::function<void(int)> cb)
    {
        rx_cb_ = cb;
    }

    /**
     * @brief Get list of supported VCP chipsets
     *
     * @return String describing supported chipsets
     */
    static const char *getSupportedChipsets()
    {
        return "FTDI (FT232/FT2232/FT4232/FT230X/FT231X/FT234XD), "
               "Silicon Labs CP210x (CP2102/CP2103/CP2104/CP2105/CP2108), "
               "WCH CH34x (CH340/CH341)";
    }

protected:
    static constexpr const char *TAG = "USBVCPDevice";

    std::unique_ptr<CdcAcmDevice> vcp_device_;
    QueueHandle_t rx_queue_;
    bool device_connected_;
    SemaphoreHandle_t disconnect_sem_;
    TaskHandle_t usb_task_handle_;

    std::function<void()> connect_cb_;
    std::function<void()> disconnect_cb_;
    std::function<void(int)> rx_cb_;

    bool drivers_registered_;

    static bool usb_host_installed_;
    static bool cdc_acm_installed_;

    /**
     * @brief Register all supported VCP drivers for auto-detection
     *
     * Registers the following drivers with the VCP service:
     * - FTDI FT23x family (FT232, FT2232, FT4232, FT230X, FT231X, FT234XD)
     * - Silicon Labs CP210x family (CP2102, CP2103, CP2104, CP2105, CP2108)
     * - WCH CH34x family (CH340, CH341)
     */
    void registerVCPDrivers()
    {
        ESP_LOGI(TAG, "Registering FTDI FT23x driver");
        VCP::register_driver<FT23x>();

        ESP_LOGI(TAG, "Registering Silicon Labs CP210x driver");
        VCP::register_driver<CP210x>();

        ESP_LOGI(TAG, "Registering WCH CH34x driver");
        VCP::register_driver<CH34x>();

        ESP_LOGI(TAG, "VCP driver registration complete");
        ESP_LOGI(TAG, "Supported chipsets: %s", getSupportedChipsets());
    }

    /**
     * @brief USB library event handling task
     */
    static void usb_lib_task_wrapper(void *arg)
    {
        while (true)
        {
            uint32_t event_flags;
            usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
            {
                usb_host_device_free_all();
            }
            if (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)
            {
                ESP_LOGD(TAG, "USB: All devices freed");
            }
        }
    }

    /**
     * @brief Data received callback
     */
    static bool data_callback_wrapper(const uint8_t *data, size_t data_len, void *arg)
    {
        USBVCPDevice *self = static_cast<USBVCPDevice *>(arg);
        if (!self || !self->rx_queue_)
        {
            return true;
        }

        // Store data in queue byte by byte
        for (size_t i = 0; i < data_len; i++)
        {
            if (xQueueSend(self->rx_queue_, &data[i], 0) != pdTRUE)
            {
                ESP_LOGW(TAG, "RX queue full, data dropped");
                break;
            }
        }

        // Trigger RX callback
        if (self->rx_cb_)
        {
            self->rx_cb_(data_len);
        }

        return true;
    }

    /**
     * @brief Device event callback
     */
    static void event_callback_wrapper(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
    {
        USBVCPDevice *self = static_cast<USBVCPDevice *>(user_ctx);
        if (!self)
            return;

        switch (event->type)
        {
        case CDC_ACM_HOST_ERROR:
            ESP_LOGE(TAG, "CDC-ACM error: %d", event->data.error);
            break;

        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            ESP_LOGI(TAG, "Device disconnected");
            self->device_connected_ = false;

            // Close the device properly to avoid invalid state errors
            if (self->vcp_device_)
            {
                ESP_LOGI(TAG, "Releasing VCP device resources...");
                self->vcp_device_.reset();
            }

            if (self->disconnect_cb_)
            {
                self->disconnect_cb_();
            }

            if (self->disconnect_sem_)
            {
                xSemaphoreGive(self->disconnect_sem_);
            }
            break;

        case CDC_ACM_HOST_SERIAL_STATE:
            ESP_LOGD(TAG, "Serial state: 0x%04X", event->data.serial_state.val);
            break;

        case CDC_ACM_HOST_NETWORK_CONNECTION:
        default:
            break;
        }
    }

    /**
     * @brief Cleanup resources
     */
    void cleanup()
    {
        if (rx_queue_)
        {
            vQueueDelete(rx_queue_);
            rx_queue_ = nullptr;
        }

        if (disconnect_sem_)
        {
            vSemaphoreDelete(disconnect_sem_);
            disconnect_sem_ = nullptr;
        }
    }
};

