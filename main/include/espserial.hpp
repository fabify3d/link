#ifndef ESP_SERIAL_H
#define ESP_SERIAL_H

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <functional>

#include "driver/gpio.h"
#include "driver/uart.h"

class ESPSerial
{
private:
    uart_port_t port;
    bool initialized;
    static const size_t BUF_SIZE = 1024;

    // Message handling
    std::function<void(const char *)> messageCallback;
    char messageBuffer[1024];
    size_t bufferIndex;
    char messageTerminator;

public:
    ESPSerial(uart_port_t uart_num = UART_NUM_0)
        : port(uart_num), initialized(false), messageCallback(nullptr),
          bufferIndex(0), messageTerminator('\n') {}

    // Initialize UART with default 8N1 configuration
    bool begin(unsigned long baud, uint32_t config = SERIAL_8N1, int8_t rxPin = -1, int8_t txPin = -1)
    {
        uart_config_t uart_config = {};
        uart_config.baud_rate = (int)baud;
        uart_config.data_bits = UART_DATA_8_BITS;
        uart_config.parity = UART_PARITY_DISABLE;
        uart_config.stop_bits = UART_STOP_BITS_1;
        uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart_config.rx_flow_ctrl_thresh = 122;
        uart_config.source_clk = UART_SCLK_DEFAULT;

        // Parse config parameter (Arduino style)
        switch (config)
        {
        case SERIAL_5N1:
            uart_config.data_bits = UART_DATA_5_BITS;
            break;
        case SERIAL_6N1:
            uart_config.data_bits = UART_DATA_6_BITS;
            break;
        case SERIAL_7N1:
            uart_config.data_bits = UART_DATA_7_BITS;
            break;
        case SERIAL_8N1:
            uart_config.data_bits = UART_DATA_8_BITS;
            break;
        case SERIAL_5N2:
            uart_config.data_bits = UART_DATA_5_BITS;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_6N2:
            uart_config.data_bits = UART_DATA_6_BITS;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_7N2:
            uart_config.data_bits = UART_DATA_7_BITS;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_8N2:
            uart_config.data_bits = UART_DATA_8_BITS;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_5E1:
            uart_config.data_bits = UART_DATA_5_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            break;
        case SERIAL_6E1:
            uart_config.data_bits = UART_DATA_6_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            break;
        case SERIAL_7E1:
            uart_config.data_bits = UART_DATA_7_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            break;
        case SERIAL_8E1:
            uart_config.data_bits = UART_DATA_8_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            break;
        case SERIAL_5E2:
            uart_config.data_bits = UART_DATA_5_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_6E2:
            uart_config.data_bits = UART_DATA_6_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_7E2:
            uart_config.data_bits = UART_DATA_7_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_8E2:
            uart_config.data_bits = UART_DATA_8_BITS;
            uart_config.parity = UART_PARITY_EVEN;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_5O1:
            uart_config.data_bits = UART_DATA_5_BITS;
            uart_config.parity = UART_PARITY_ODD;
            break;
        case SERIAL_6O1:
            uart_config.data_bits = UART_DATA_6_BITS;
            uart_config.parity = UART_PARITY_ODD;
            break;
        case SERIAL_7O1:
            uart_config.data_bits = UART_DATA_7_BITS;
            uart_config.parity = UART_PARITY_ODD;
            break;
        case SERIAL_8O1:
            uart_config.data_bits = UART_DATA_8_BITS;
            uart_config.parity = UART_PARITY_ODD;
            break;
        case SERIAL_5O2:
            uart_config.data_bits = UART_DATA_5_BITS;
            uart_config.parity = UART_PARITY_ODD;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_6O2:
            uart_config.data_bits = UART_DATA_6_BITS;
            uart_config.parity = UART_PARITY_ODD;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_7O2:
            uart_config.data_bits = UART_DATA_7_BITS;
            uart_config.parity = UART_PARITY_ODD;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        case SERIAL_8O2:
            uart_config.data_bits = UART_DATA_8_BITS;
            uart_config.parity = UART_PARITY_ODD;
            uart_config.stop_bits = UART_STOP_BITS_2;
            break;
        }

        if (uart_param_config(port, &uart_config) != ESP_OK)
            return false;

        // Set pins if specified
        if (rxPin >= 0 && txPin >= 0)
        {
            if (uart_set_pin(port, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK)
                return false;
        }

        if (uart_driver_install(port, BUF_SIZE * 2, 0, 0, NULL, 0) != ESP_OK)
            return false;

        initialized = true;
        bufferIndex = 0;
        return true;
    }

    void end()
    {
        if (initialized)
        {
            uart_driver_delete(port);
            initialized = false;
            messageCallback = nullptr;
        }
    }

    // Check if initialized
    bool isInitialized() const
    {
        return initialized;
    }

    // Set message callback with custom terminator (default is '\n')
    void onMessage(std::function<void(const char *)> callback, char terminator = '\n')
    {
        messageCallback = callback;
        messageTerminator = terminator;
        bufferIndex = 0;
    }

    // Send a message with automatic newline
    size_t sendMessage(const char *message)
    {
        if (!initialized || message == nullptr)
            return 0;

        size_t len = strlen(message);
        size_t written = write((const uint8_t *)message, len);
        written += write((uint8_t)'\n');
        return written;
    }

    // Overload for std::string
    size_t sendMessage(const std::string &message)
    {
        return sendMessage(message.c_str());
    }

    // Process incoming data and trigger callback when message is complete
    void loop()
    {
        if (!initialized || messageCallback == nullptr)
            return;

        while (available() > 0)
        {
            int c = read();
            if (c < 0)
                break;

            // Check for terminator
            if ((char)c == messageTerminator)
            {
                if (bufferIndex > 0)
                {
                    messageBuffer[bufferIndex] = '\0';
                    messageCallback(messageBuffer);
                    bufferIndex = 0;
                }
            }
            else if (c == '\r')
            {
                // Ignore carriage return
                continue;
            }
            else
            {
                // Add to buffer if space available
                if (bufferIndex < sizeof(messageBuffer) - 1)
                {
                    messageBuffer[bufferIndex++] = (char)c;
                }
                else
                {
                    // Buffer overflow - log, reset and discard
                    messageBuffer[sizeof(messageBuffer) - 1] = '\0';
                    ESP_LOGW("ESPSerial", "Buffer overflow. Truncated content: %s", messageBuffer);
                    bufferIndex = 0;
                }
            }
        }
    }

    // Close connection (alias for end())
    void close()
    {
        end();
    }

    // Check available bytes
    int available()
    {
        size_t len = 0;
        uart_get_buffered_data_len(port, &len);
        return (int)len;
    }

    // Read single byte
    int read()
    {
        uint8_t c;
        int len = uart_read_bytes(port, &c, 1, 0);
        return (len > 0) ? c : -1;
    }

    // Peek at next byte without removing it
    int peek()
    {
        if (uart_get_buffered_data_len(port, NULL) > 0)
        {
            // There's no direct peek in ESP-IDF, so we'll read and put back
            // This is a limitation - peek isn't perfectly implemented
            return -1; // Not fully supported
        }
        return -1;
    }

    // Read bytes into buffer
    size_t readBytes(char *buffer, size_t length)
    {
        return uart_read_bytes(port, (uint8_t *)buffer, length, 20 / portTICK_PERIOD_MS);
    }

    // Read until terminator
    size_t readBytesUntil(char terminator, char *buffer, size_t length)
    {
        size_t idx = 0;
        while (idx < length - 1)
        {
            int c = read();
            if (c < 0)
                break;
            if (c == terminator)
                break;
            buffer[idx++] = (char)c;
        }
        buffer[idx] = '\0';
        return idx;
    }

    // Read line (until \n)
    std::string readStringUntil(char terminator)
    {
        char buffer[256];
        size_t len = readBytesUntil(terminator, buffer, sizeof(buffer));
        return std::string(buffer, len);
    }

    // Write single byte
    size_t write(uint8_t c)
    {
        return uart_write_bytes(port, (const char *)&c, 1);
    }

    // Write buffer
    size_t write(const uint8_t *buffer, size_t size)
    {
        return uart_write_bytes(port, (const char *)buffer, size);
    }

    // Print string
    size_t print(const char *str)
    {
        return write((const uint8_t *)str, strlen(str));
    }

    size_t print(int n, int base = 10)
    {
        char buf[16];
        if (base == 10)
        {
            snprintf(buf, sizeof(buf), "%d", n);
        }
        else if (base == 16)
        {
            snprintf(buf, sizeof(buf), "%x", n);
        }
        else if (base == 8)
        {
            snprintf(buf, sizeof(buf), "%o", n);
        }
        else if (base == 2)
        {
            // Binary conversion
            int i = 0;
            unsigned int num = (n < 0) ? -n : n;
            if (num == 0)
            {
                buf[i++] = '0';
            }
            else
            {
                char temp[16];
                int j = 0;
                while (num > 0)
                {
                    temp[j++] = '0' + (num & 1);
                    num >>= 1;
                }
                if (n < 0)
                    buf[i++] = '-';
                while (j > 0)
                {
                    buf[i++] = temp[--j];
                }
            }
            buf[i] = '\0';
            return print(buf);
        }
        return print(buf);
    }

    size_t print(unsigned int n, int base = 10)
    {
        char buf[16];
        if (base == 10)
        {
            snprintf(buf, sizeof(buf), "%u", n);
        }
        else if (base == 16)
        {
            snprintf(buf, sizeof(buf), "%x", n);
        }
        else if (base == 8)
        {
            snprintf(buf, sizeof(buf), "%o", n);
        }
        else if (base == 2)
        {
            // Binary conversion
            int i = 0;
            if (n == 0)
            {
                buf[i++] = '0';
            }
            else
            {
                char temp[16];
                int j = 0;
                while (n > 0)
                {
                    temp[j++] = '0' + (n & 1);
                    n >>= 1;
                }
                while (j > 0)
                {
                    buf[i++] = temp[--j];
                }
            }
            buf[i] = '\0';
            return print(buf);
        }
        return print(buf);
    }

    size_t print(long n, int base = 10)
    {
        char buf[32];
        if (base == 10)
        {
            snprintf(buf, sizeof(buf), "%ld", n);
        }
        else if (base == 16)
        {
            snprintf(buf, sizeof(buf), "%lx", n);
        }
        else if (base == 8)
        {
            snprintf(buf, sizeof(buf), "%lo", n);
        }
        else if (base == 2)
        {
            // Binary conversion
            int i = 0;
            unsigned long num = (n < 0) ? -n : n;
            if (num == 0)
            {
                buf[i++] = '0';
            }
            else
            {
                char temp[32];
                int j = 0;
                while (num > 0)
                {
                    temp[j++] = '0' + (num & 1);
                    num >>= 1;
                }
                if (n < 0)
                    buf[i++] = '-';
                while (j > 0)
                {
                    buf[i++] = temp[--j];
                }
            }
            buf[i] = '\0';
            return print(buf);
        }
        return print(buf);
    }

    size_t print(unsigned long n, int base = 10)
    {
        char buf[32];
        if (base == 10)
        {
            snprintf(buf, sizeof(buf), "%lu", n);
        }
        else if (base == 16)
        {
            snprintf(buf, sizeof(buf), "%lx", n);
        }
        else if (base == 8)
        {
            snprintf(buf, sizeof(buf), "%lo", n);
        }
        else if (base == 2)
        {
            // Binary conversion
            int i = 0;
            if (n == 0)
            {
                buf[i++] = '0';
            }
            else
            {
                char temp[32];
                int j = 0;
                while (n > 0)
                {
                    temp[j++] = '0' + (n & 1);
                    n >>= 1;
                }
                while (j > 0)
                {
                    buf[i++] = temp[--j];
                }
            }
            buf[i] = '\0';
            return print(buf);
        }
        return print(buf);
    }

    size_t print(double n, int digits = 2)
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", digits, n);
        return print(buf);
    }

    // Print with newline
    size_t println(const char *str)
    {
        size_t n = print(str);
        n += print("\r\n");
        return n;
    }

    size_t println(int n, int base = 10)
    {
        size_t s = print(n, base);
        s += println();
        return s;
    }

    size_t println(unsigned int n, int base = 10)
    {
        size_t s = print(n, base);
        s += println();
        return s;
    }

    size_t println(long n, int base = 10)
    {
        size_t s = print(n, base);
        s += println();
        return s;
    }

    size_t println(unsigned long n, int base = 10)
    {
        size_t s = print(n, base);
        s += println();
        return s;
    }

    size_t println(double n, int digits = 2)
    {
        size_t s = print(n, digits);
        s += println();
        return s;
    }

    size_t println()
    {
        return print("\r\n");
    }

    // Printf-style formatting
    size_t printf(const char *format, ...)
    {
        char buf[256];
        va_list args;
        va_start(args, format);
        int len = vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);
        if (len > 0)
        {
            return write((const uint8_t *)buf, len);
        }
        return 0;
    }

    // Flush TX buffer
    void flush()
    {
        uart_wait_tx_done(port, 100 / portTICK_PERIOD_MS);
    }

    // Arduino Serial config constants
    static const uint32_t SERIAL_5N1 = 0x00;
    static const uint32_t SERIAL_6N1 = 0x01;
    static const uint32_t SERIAL_7N1 = 0x02;
    static const uint32_t SERIAL_8N1 = 0x03;
    static const uint32_t SERIAL_5N2 = 0x04;
    static const uint32_t SERIAL_6N2 = 0x05;
    static const uint32_t SERIAL_7N2 = 0x06;
    static const uint32_t SERIAL_8N2 = 0x07;
    static const uint32_t SERIAL_5E1 = 0x08;
    static const uint32_t SERIAL_6E1 = 0x09;
    static const uint32_t SERIAL_7E1 = 0x0A;
    static const uint32_t SERIAL_8E1 = 0x0B;
    static const uint32_t SERIAL_5E2 = 0x0C;
    static const uint32_t SERIAL_6E2 = 0x0D;
    static const uint32_t SERIAL_7E2 = 0x0E;
    static const uint32_t SERIAL_8E2 = 0x0F;
    static const uint32_t SERIAL_5O1 = 0x10;
    static const uint32_t SERIAL_6O1 = 0x11;
    static const uint32_t SERIAL_7O1 = 0x12;
    static const uint32_t SERIAL_8O1 = 0x13;
    static const uint32_t SERIAL_5O2 = 0x14;
    static const uint32_t SERIAL_6O2 = 0x15;
    static const uint32_t SERIAL_7O2 = 0x16;
    static const uint32_t SERIAL_8O2 = 0x17;
};

// Create global instances for convenience
extern ESPSerial Serial;  // UART0 (usually USB/debug)
extern ESPSerial Serial1; // UART1
extern ESPSerial Serial2; // UART2

#endif // ESP_SERIAL_H