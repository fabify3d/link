// storage_manager.h
// Header-only LittleFS storage manager for ESP-IDF
// Usage: Just #include this file in your main.cpp

#ifndef STORAGE_MANAGER_H
#define STORAGE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"

// ============================================================================
// Configuration
// ============================================================================
#ifndef STORAGE_BASE_PATH
#define STORAGE_BASE_PATH "/littlefs"
#endif

#ifndef STORAGE_PARTITION_LABEL
#define STORAGE_PARTITION_LABEL "storage"
#endif

#ifndef STORAGE_MAX_KEY_LENGTH
#define STORAGE_MAX_KEY_LENGTH 64
#endif

#ifndef STORAGE_MAX_PATH_LENGTH
#define STORAGE_MAX_PATH_LENGTH 128
#endif

#ifndef STORAGE_STRING_BUFFER_SIZE
#define STORAGE_STRING_BUFFER_SIZE 512
#endif

// ============================================================================
// Internal State
// ============================================================================
namespace
{
    static const char *STORAGE_TAG = "storage_manager";
    static const char *STORAGE_DIR_PATH = STORAGE_BASE_PATH "/kv";
    static bool storage_initialized = false;
}

// ============================================================================
// Helper Functions
// ============================================================================
namespace StorageInternal
{

    inline bool validate_key(const char *key)
    {
        if (key == NULL || key[0] == '\0')
        {
            ESP_LOGE(STORAGE_TAG, "Invalid key: NULL or empty");
            return false;
        }

        size_t len = strlen(key);
        if (len > STORAGE_MAX_KEY_LENGTH)
        {
            ESP_LOGE(STORAGE_TAG, "Key too long: %d > %d", len, STORAGE_MAX_KEY_LENGTH);
            return false;
        }

        // Check for invalid characters (/, \, etc.)
        for (size_t i = 0; i < len; i++)
        {
            char c = key[i];
            if (c == '/' || c == '\\' || c == '\0' || c < 32 || c > 126)
            {
                ESP_LOGE(STORAGE_TAG, "Invalid character in key: 0x%02X", c);
                return false;
            }
        }

        return true;
    }

    inline bool get_file_path(const char *key, char *path, size_t path_size)
    {
        if (!validate_key(key))
        {
            return false;
        }

        int ret = snprintf(path, path_size, "%s/%s", STORAGE_DIR_PATH, key);
        if (ret < 0 || ret >= (int)path_size)
        {
            ESP_LOGE(STORAGE_TAG, "Path buffer too small");
            return false;
        }

        return true;
    }

    inline bool file_exists(const char *path)
    {
        struct stat st;
        return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    }

    inline bool dir_exists(const char *path)
    {
        struct stat st;
        return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    }

    inline bool create_directory_if_needed(const char *path)
    {
        if (dir_exists(path))
        {
            return true;
        }

        if (mkdir(path, 0755) != 0)
        {
            ESP_LOGE(STORAGE_TAG, "Failed to create directory: %s", path);
            return false;
        }

        ESP_LOGD(STORAGE_TAG, "Created directory: %s", path);
        return true;
    }
}

// ============================================================================
// Public API
// ============================================================================

/**
 * @brief Initialize the storage manager
 * @return ESP_OK on success, error code otherwise
 */
inline esp_err_t storage_init(void)
{
    if (storage_initialized)
    {
        ESP_LOGW(STORAGE_TAG, "Storage already initialized");
        return ESP_OK;
    }

    ESP_LOGI(STORAGE_TAG, "Initializing storage manager...");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = STORAGE_BASE_PATH,
        .partition_label = STORAGE_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(STORAGE_TAG, "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE(STORAGE_TAG, "LittleFS partition '%s' not found", STORAGE_PARTITION_LABEL);
        }
        else
        {
            ESP_LOGE(STORAGE_TAG, "Failed to initialize LittleFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    // Create storage directory if needed
    if (!StorageInternal::create_directory_if_needed(STORAGE_DIR_PATH))
    {
        ESP_LOGE(STORAGE_TAG, "Failed to create storage directory");
        esp_vfs_littlefs_unregister(STORAGE_PARTITION_LABEL);
        return ESP_FAIL;
    }

    // Log filesystem info
    size_t total = 0, used = 0;
    ret = esp_littlefs_info(STORAGE_PARTITION_LABEL, &total, &used);
    if (ret == ESP_OK)
    {
        ESP_LOGI(STORAGE_TAG, "Filesystem: %d KB total, %d KB used (%.1f%%)",
                 total / 1024, used / 1024, (used * 100.0) / total);
    }
    else
    {
        ESP_LOGW(STORAGE_TAG, "Could not get filesystem info");
    }

    storage_initialized = true;
    ESP_LOGI(STORAGE_TAG, "Storage manager initialized successfully");
    return ESP_OK;
}

/**
 * @brief Deinitialize and unmount storage
 * @return ESP_OK on success
 */
inline esp_err_t storage_deinit(void)
{
    if (!storage_initialized)
    {
        ESP_LOGW(STORAGE_TAG, "Storage not initialized");
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_littlefs_unregister(STORAGE_PARTITION_LABEL);
    if (ret == ESP_OK)
    {
        storage_initialized = false;
        ESP_LOGI(STORAGE_TAG, "Storage manager deinitialized");
    }
    else
    {
        ESP_LOGE(STORAGE_TAG, "Failed to deinitialize: %s", esp_err_to_name(ret));
    }

    return ret;
}

/**
 * @brief Check if storage is initialized
 * @return true if initialized
 */
inline bool storage_is_initialized(void)
{
    return storage_initialized;
}

/**
 * @brief Set a string value (copies the string to provided buffer)
 * @param key Storage key
 * @param buffer Buffer to store the retrieved string
 * @param buffer_size Size of the buffer
 * @return ESP_OK on success, error code otherwise
 */
inline esp_err_t storage_get_string(const char *key, char *buffer, size_t buffer_size)
{
    if (!storage_initialized)
    {
        ESP_LOGE(STORAGE_TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == NULL || buffer_size == 0)
    {
        ESP_LOGE(STORAGE_TAG, "Invalid buffer");
        return ESP_ERR_INVALID_ARG;
    }

    char path[STORAGE_MAX_PATH_LENGTH];
    if (!StorageInternal::get_file_path(key, path, sizeof(path)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!StorageInternal::file_exists(path))
    {
        ESP_LOGD(STORAGE_TAG, "Key not found: %s", key);
        buffer[0] = '\0';
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to open file: %s", path);
        buffer[0] = '\0';
        return ESP_FAIL;
    }

    // Read file content
    size_t read = fread(buffer, 1, buffer_size - 1, f);
    buffer[read] = '\0';

    bool truncated = false;
    if (!feof(f))
    {
        truncated = true;
        ESP_LOGW(STORAGE_TAG, "String truncated for key: %s", key);
    }

    fclose(f);

    ESP_LOGD(STORAGE_TAG, "Get string: %s = %s", key, buffer);
    return truncated ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

/**
 * @brief Set a string value
 * @param key Storage key
 * @param value String value to store
 * @return ESP_OK on success
 */
inline esp_err_t storage_set_string(const char *key, const char *value)
{
    if (!storage_initialized)
    {
        ESP_LOGE(STORAGE_TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (value == NULL)
    {
        ESP_LOGE(STORAGE_TAG, "NULL value");
        return ESP_ERR_INVALID_ARG;
    }

    char path[STORAGE_MAX_PATH_LENGTH];
    if (!StorageInternal::get_file_path(key, path, sizeof(path)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "w");
    if (f == NULL)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to open file for writing: %s", path);
        return ESP_FAIL;
    }

    size_t len = strlen(value);
    size_t written = fwrite(value, 1, len, f);
    fclose(f);

    if (written != len)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to write complete data (wrote %d of %d bytes)", written, len);
        unlink(path); // Clean up partial write
        return ESP_FAIL;
    }

    ESP_LOGD(STORAGE_TAG, "Set string: %s = %s", key, value);
    return ESP_OK;
}

/**
 * @brief Set an integer value
 * @param key Storage key
 * @param value Integer value to store
 * @return ESP_OK on success
 */
inline esp_err_t storage_set_int(const char *key, int32_t value)
{
    char str[16];
    snprintf(str, sizeof(str), "%ld", (long)value);
    return storage_set_string(key, str);
}

/**
 * @brief Get an integer value
 * @param key Storage key
 * @param default_value Default value if key doesn't exist
 * @param result Pointer to store the result (optional, can be NULL)
 * @return The value, or default_value if not found
 */
inline int32_t storage_get_int(const char *key, int32_t default_value, esp_err_t *result = nullptr)
{
    char buffer[16];
    esp_err_t ret = storage_get_string(key, buffer, sizeof(buffer));

    if (result != nullptr)
    {
        *result = ret;
    }

    if (ret == ESP_OK)
    {
        return (int32_t)atol(buffer);
    }

    return default_value;
}

/**
 * @brief Set a float value
 * @param key Storage key
 * @param value Float value to store
 * @return ESP_OK on success
 */
inline esp_err_t storage_set_float(const char *key, float value)
{
    char str[32];
    snprintf(str, sizeof(str), "%.6f", value);
    return storage_set_string(key, str);
}

/**
 * @brief Get a float value
 * @param key Storage key
 * @param default_value Default value if key doesn't exist
 * @param result Pointer to store the result (optional, can be NULL)
 * @return The value, or default_value if not found
 */
inline float storage_get_float(const char *key, float default_value, esp_err_t *result = nullptr)
{
    char buffer[32];
    esp_err_t ret = storage_get_string(key, buffer, sizeof(buffer));

    if (result != nullptr)
    {
        *result = ret;
    }

    if (ret == ESP_OK)
    {
        return atof(buffer);
    }

    return default_value;
}

/**
 * @brief Set a boolean value
 * @param key Storage key
 * @param value Boolean value to store
 * @return ESP_OK on success
 */
inline esp_err_t storage_set_bool(const char *key, bool value)
{
    return storage_set_string(key, value ? "1" : "0");
}

/**
 * @brief Get a boolean value
 * @param key Storage key
 * @param default_value Default value if key doesn't exist
 * @param result Pointer to store the result (optional, can be NULL)
 * @return The value, or default_value if not found
 */
inline bool storage_get_bool(const char *key, bool default_value, esp_err_t *result = nullptr)
{
    char buffer[2];
    esp_err_t ret = storage_get_string(key, buffer, sizeof(buffer));

    if (result != nullptr)
    {
        *result = ret;
    }

    if (ret == ESP_OK)
    {
        return (buffer[0] == '1');
    }

    return default_value;
}

/**
 * @brief Set binary data
 * @param key Storage key
 * @param data Pointer to binary data
 * @param length Length of data in bytes
 * @return ESP_OK on success
 */
inline esp_err_t storage_set_blob(const char *key, const void *data, size_t length)
{
    if (!storage_initialized)
    {
        ESP_LOGE(STORAGE_TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || length == 0)
    {
        ESP_LOGE(STORAGE_TAG, "Invalid data or length");
        return ESP_ERR_INVALID_ARG;
    }

    char path[STORAGE_MAX_PATH_LENGTH];
    if (!StorageInternal::get_file_path(key, path, sizeof(path)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to open file for writing: %s", path);
        return ESP_FAIL;
    }

    size_t written = fwrite(data, 1, length, f);
    fclose(f);

    if (written != length)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to write complete data (wrote %d of %d bytes)", written, length);
        unlink(path); // Clean up partial write
        return ESP_FAIL;
    }

    ESP_LOGD(STORAGE_TAG, "Set blob: %s (%d bytes)", key, length);
    return ESP_OK;
}

/**
 * @brief Get binary data (copies to provided buffer)
 * @param key Storage key
 * @param buffer Buffer to store the data
 * @param buffer_size Size of the buffer
 * @param actual_length Pointer to store actual data length (optional)
 * @return ESP_OK on success
 */
inline esp_err_t storage_get_blob(const char *key, void *buffer, size_t buffer_size, size_t *actual_length = nullptr)
{
    if (!storage_initialized)
    {
        ESP_LOGE(STORAGE_TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (buffer == NULL || buffer_size == 0)
    {
        ESP_LOGE(STORAGE_TAG, "Invalid buffer");
        return ESP_ERR_INVALID_ARG;
    }

    char path[STORAGE_MAX_PATH_LENGTH];
    if (!StorageInternal::get_file_path(key, path, sizeof(path)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!StorageInternal::file_exists(path))
    {
        ESP_LOGD(STORAGE_TAG, "Key not found: %s", key);
        return ESP_ERR_NOT_FOUND;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to open file: %s", path);
        return ESP_FAIL;
    }

    // Read data
    size_t read = fread(buffer, 1, buffer_size, f);

    bool truncated = false;
    if (!feof(f))
    {
        truncated = true;
        ESP_LOGW(STORAGE_TAG, "Blob truncated for key: %s", key);
    }

    fclose(f);

    if (actual_length != nullptr)
    {
        *actual_length = read;
    }

    ESP_LOGD(STORAGE_TAG, "Get blob: %s (%d bytes)", key, read);
    return truncated ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

/**
 * @brief Delete a key
 * @param key Storage key
 * @return ESP_OK on success
 */
inline esp_err_t storage_delete(const char *key)
{
    if (!storage_initialized)
    {
        ESP_LOGE(STORAGE_TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    char path[STORAGE_MAX_PATH_LENGTH];
    if (!StorageInternal::get_file_path(key, path, sizeof(path)))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!StorageInternal::file_exists(path))
    {
        ESP_LOGD(STORAGE_TAG, "Key doesn't exist: %s", key);
        return ESP_ERR_NOT_FOUND;
    }

    if (unlink(path) != 0)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to delete key: %s", key);
        return ESP_FAIL;
    }

    ESP_LOGD(STORAGE_TAG, "Deleted key: %s", key);
    return ESP_OK;
}

/**
 * @brief Check if a key exists
 * @param key Storage key
 * @return true if key exists
 */
inline bool storage_exists(const char *key)
{
    if (!storage_initialized)
    {
        return false;
    }

    if (!StorageInternal::validate_key(key))
    {
        return false;
    }

    char path[STORAGE_MAX_PATH_LENGTH];
    if (!StorageInternal::get_file_path(key, path, sizeof(path)))
    {
        return false;
    }

    return StorageInternal::file_exists(path);
}

/**
 * @brief Clear all storage
 * @return ESP_OK on success
 */
inline esp_err_t storage_clear(void)
{
    if (!storage_initialized)
    {
        ESP_LOGE(STORAGE_TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    DIR *dir = opendir(STORAGE_DIR_PATH);
    if (dir == NULL)
    {
        ESP_LOGE(STORAGE_TAG, "Failed to open storage directory");
        return ESP_FAIL;
    }

    int deleted_count = 0;
    int failed_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        char path[STORAGE_MAX_PATH_LENGTH];
        snprintf(path, sizeof(path), "%s/%s", STORAGE_DIR_PATH, entry->d_name);

        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
        {
            if (unlink(path) == 0)
            {
                deleted_count++;
            }
            else
            {
                failed_count++;
                ESP_LOGW(STORAGE_TAG, "Failed to delete: %s", entry->d_name);
            }
        }
    }

    closedir(dir);

    ESP_LOGI(STORAGE_TAG, "Storage cleared: %d deleted, %d failed", deleted_count, failed_count);
    return (failed_count == 0) ? ESP_OK : ESP_FAIL;
}

/**
 * @brief Get storage statistics
 * @param total_bytes Pointer to store total bytes (optional)
 * @param used_bytes Pointer to store used bytes (optional)
 * @return ESP_OK on success
 */
inline esp_err_t storage_get_stats(size_t *total_bytes = nullptr, size_t *used_bytes = nullptr)
{
    if (!storage_initialized)
    {
        ESP_LOGE(STORAGE_TAG, "Storage not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    size_t total = 0, used = 0;
    esp_err_t ret = esp_littlefs_info(STORAGE_PARTITION_LABEL, &total, &used);

    if (ret == ESP_OK)
    {
        if (total_bytes != nullptr)
            *total_bytes = total;
        if (used_bytes != nullptr)
            *used_bytes = used;
    }

    return ret;
}

#endif // STORAGE_MANAGER_H

// ============================================================================
// USAGE EXAMPLE FOR main.cpp
// ============================================================================
/*

#include "storage_manager.h"

extern "C" void app_main(void) {
    // Initialize storage
    ESP_ERROR_CHECK(storage_init());

    // Set values
    storage_set_string("device_name", "ESP32_Module");
    storage_set_int("boot_count", 42);
    storage_set_float("temperature", 23.5);
    storage_set_bool("wifi_enabled", true);

    // Get values - no free() needed!
    char name[64];
    if (storage_get_string("device_name", name, sizeof(name)) == ESP_OK) {
        ESP_LOGI("APP", "Device name: %s", name);
    }

    int32_t count = storage_get_int("boot_count", 0);
    ESP_LOGI("APP", "Boot count: %ld", (long)count);

    float temp = storage_get_float("temperature", 0.0);
    ESP_LOGI("APP", "Temperature: %.2f", temp);

    bool wifi = storage_get_bool("wifi_enabled", false);
    ESP_LOGI("APP", "WiFi: %s", wifi ? "enabled" : "disabled");

    // Binary data
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    storage_set_blob("config", data, sizeof(data));

    uint8_t buffer[10];
    size_t read_len;
    if (storage_get_blob("config", buffer, sizeof(buffer), &read_len) == ESP_OK) {
        ESP_LOGI("APP", "Read %d bytes", read_len);
    }

    // Check existence
    if (storage_exists("device_name")) {
        ESP_LOGI("APP", "device_name exists");
    }

    // Get statistics
    size_t total, used;
    if (storage_get_stats(&total, &used) == ESP_OK) {
        ESP_LOGI("APP", "Storage: %d/%d KB (%.1f%%)",
                 used/1024, total/1024, (used*100.0)/total);
    }

    // Delete a key
    storage_delete("boot_count");

    // Clean up
    storage_deinit();
}

*/