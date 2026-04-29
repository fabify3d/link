#ifndef MESSAGE_ROUTER_H
#define MESSAGE_ROUTER_H

#include <stdio.h>
#include <string.h>
#include <vector>
#include <functional>
#include <map>
#include <algorithm>
#include <string>
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG_ROUTER = "MSG_RTR";

class MessageRouter
{
public:
    // Lock modes for path control
    enum class LockMode
    {
        STRICT,      // Only exact sender and receiver IDs allowed
        EXCLUSIVE,   // No other sender/receiver allowed
        ANY_SENDER,  // Only receiver is locked
        ANY_RECEIVER // Only sender is locked
    };

    // Path lock structure
    struct PathLock
    {
        std::string path;
        LockMode mode;
        std::string senderID;
        std::string senderOwner;
        std::string receiverID;
        std::string receiverOwner;
        bool finalized;
        uint64_t timestamp;

        PathLock() : mode(LockMode::STRICT), finalized(false), timestamp(0) {}

        PathLock(const std::string &p, LockMode m, const std::string &senderId = "", const std::string &receiverId = "")
            : path(p), mode(m), senderID(senderId), receiverID(receiverId),
              finalized(false), timestamp(esp_timer_get_time() / 1000) {}
    };

    // Enhanced message structure with sender information
    struct Message
    {
        std::string path;
        std::string data;
        std::string senderID;
        std::string receiverID;
        uint64_t timestamp;
        uint32_t messageId;

        Message(const std::string &p, const std::string &d, const std::string &sender = "", const std::string &receiver = "")
            : path(p), data(d), senderID(sender), receiverID(receiver),
              timestamp(esp_timer_get_time() / 1000), messageId(0) {}
    };

    // Enhanced callback function type with sender information
    typedef std::function<void(const Message &, const std::string &, const std::string &)> MessageCallback;

private:
    // Enhanced listener structure with ID and metadata
    struct Listener
    {
        std::string path;
        MessageCallback callback;
        uint32_t id;
        std::string ownerID;
        bool active;
        uint64_t createdAt;

        Listener(const std::string &p, MessageCallback cb, uint32_t i, const std::string &owner = "")
            : path(p), callback(cb), id(i), ownerID(owner), active(true), createdAt(esp_timer_get_time() / 1000) {}
    };

    // Enhanced message queue structure
    struct QueuedMessage
    {
        Message message;
        std::string senderPath;
        uint32_t priority;

        QueuedMessage(const Message &msg, const std::string &sender, uint32_t prio = 0)
            : message(msg), senderPath(sender), priority(prio) {}
    };

    std::vector<Listener> listeners;
    std::vector<QueuedMessage> messageQueue;
    std::map<std::string, PathLock> pathLocks;
    SemaphoreHandle_t _mutex;
    uint32_t nextListenerId;
    uint32_t nextMessageId;
    static const size_t MAX_QUEUE_SIZE = 50;

    // Helper function to normalize paths
    std::string normalizePath(const std::string &path) const
    {
        std::string normalized = path;

        // Remove trailing slash except for root
        if (normalized.length() > 1 && normalized.back() == '/')
        {
            normalized = normalized.substr(0, normalized.length() - 1);
        }

        // Ensure path starts with /
        if (normalized.empty() || normalized[0] != '/')
        {
            normalized = "/" + normalized;
        }

        return normalized;
    }

    // Get parent path for bubbling
    std::string getParentPath(const std::string &path)
    {
        if (path == "/")
            return "";

        size_t lastSlash = path.rfind('/');
        if (lastSlash == std::string::npos || lastSlash == 0)
            return "/";

        return path.substr(0, lastSlash);
    }

    // Get all paths that should receive the message
    std::vector<std::string> getBubblingPaths(const std::string &path)
    {
        std::vector<std::string> paths;
        std::string currentPath = normalizePath(path);

        paths.push_back(currentPath);

        while (currentPath != "/")
        {
            currentPath = getParentPath(currentPath);
            if (!currentPath.empty())
            {
                paths.push_back(currentPath);
            }
        }

        return paths;
    }

    // Validate message against path locks
    bool validateMessageLock(const Message &message)
    {
        std::string normalizedPath = normalizePath(message.path);

        // Check if path is locked
        auto lockIt = pathLocks.find(normalizedPath);
        if (lockIt == pathLocks.end())
        {
            return true; // No lock, message allowed
        }

        const PathLock &lock = lockIt->second;

        switch (lock.mode)
        {
        case LockMode::STRICT:
            // Must match exact sender and receiver IDs
            if (!lock.senderID.empty() && lock.senderID != message.senderID)
                return false;
            if (!lock.receiverID.empty() && lock.receiverID != message.receiverID)
                return false;
            break;

        case LockMode::EXCLUSIVE:
            // No other sender/receiver allowed if lock is set
            if (!lock.senderID.empty() && lock.senderID != message.senderID)
                return false;
            if (!lock.receiverID.empty() && lock.receiverID != message.receiverID)
                return false;
            break;

        case LockMode::ANY_SENDER:
            // Only receiver is locked
            if (!lock.receiverID.empty() && lock.receiverID != message.receiverID)
                return false;
            break;

        case LockMode::ANY_RECEIVER:
            // Only sender is locked
            if (!lock.senderID.empty() && lock.senderID != message.senderID)
                return false;
            break;
        }

        return true;
    }

    // Process a single message immediately
    void processMessageImmediate(const Message &message, const std::string &senderPath = "")
    {
        // Validate against path locks
        if (!validateMessageLock(message))
        {
            ESP_LOGW(TAG_ROUTER, "Message blocked by path lock: %s", message.path.c_str());
            return;
        }

        std::vector<std::string> targetPaths = getBubblingPaths(message.path);

        // Send to all matching active listeners
        for (const std::string &targetPath : targetPaths)
        {
            for (const Listener &listener : listeners)
            {
                if (listener.path == targetPath && listener.active)
                {
                    // Check if message is targeted to specific receiver
                    if (!message.receiverID.empty())
                    {
                        char idStr[16];
                        snprintf(idStr, sizeof(idStr), "%lu", listener.id);
                        if (std::string(idStr) != message.receiverID)
                        {
                            continue; // Skip if not the intended receiver
                        }
                    }

                    try
                    {
                        // Call callback with enhanced parameters: (message, path, senderID)
                        listener.callback(message, targetPath, message.senderID);
                    }
                    catch (...)
                    {
                        ESP_LOGW(TAG_ROUTER, "Listener callback threw exception");
                    }
                }
            }
        }
    }

public:
    MessageRouter() : nextListenerId(1), nextMessageId(1) {
        _mutex = xSemaphoreCreateRecursiveMutex();
    }

    ~MessageRouter() {
        if (_mutex) {
            vSemaphoreDelete(_mutex);
        }
    }

    // Enhanced listener management with ID-based operations
    uint32_t addListener(const std::string &path, MessageCallback callback, const std::string &ownerID = "")
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        std::string normalizedPath = normalizePath(path);
        uint32_t id = nextListenerId++;

        listeners.emplace_back(normalizedPath, callback, id, ownerID);

        ESP_LOGI(TAG_ROUTER, "Listener %u added to path: %s (owner: %s)",
                 id, normalizedPath.c_str(), ownerID.c_str());
        xSemaphoreGiveRecursive(_mutex);
        return id;
    }

    // Remove listener by ID
    bool removeListener(uint32_t listenerId)
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        for (auto it = listeners.begin(); it != listeners.end(); ++it)
        {
            if (it->id == listenerId)
            {
                ESP_LOGI(TAG_ROUTER, "Listener %u removed from path: %s", listenerId, it->path.c_str());
                listeners.erase(it);
                xSemaphoreGiveRecursive(_mutex);
                return true;
            }
        }
        xSemaphoreGiveRecursive(_mutex);
        return false;
    }

    // Remove multiple listeners by ID
    int removeListeners(const std::vector<uint32_t> &listenerIds)
    {
        int removedCount = 0;

        for (uint32_t id : listenerIds)
        {
            if (removeListener(id))
            {
                removedCount++;
            }
        }

        return removedCount;
    }

    // Remove all listeners from a specific path
    int removeListenersByPath(const std::string &path)
    {
        std::string normalizedPath = normalizePath(path);
        int removedCount = 0;

        auto it = listeners.begin();
        while (it != listeners.end())
        {
            if (it->path == normalizedPath)
            {
                ESP_LOGI(TAG_ROUTER, "Listener %u removed from path: %s", it->id, normalizedPath.c_str());
                it = listeners.erase(it);
                removedCount++;
            }
            else
            {
                ++it;
            }
        }

        return removedCount;
    }

    // Remove listeners by owner ID
    int removeListenersByOwner(const std::string &ownerID)
    {
        int removedCount = 0;

        auto it = listeners.begin();
        while (it != listeners.end())
        {
            if (it->ownerID == ownerID)
            {
                ESP_LOGI(TAG_ROUTER, "Listener %u removed (owner: %s)", it->id, ownerID.c_str());
                it = listeners.erase(it);
                removedCount++;
            }
            else
            {
                ++it;
            }
        }

        return removedCount;
    }

    // Activate/Deactivate listener without removing
    bool setListenerActive(uint32_t listenerId, bool active)
    {
        for (Listener &listener : listeners)
        {
            if (listener.id == listenerId)
            {
                listener.active = active;
                ESP_LOGI(TAG_ROUTER, "Listener %u %s", listenerId, active ? "activated" : "deactivated");
                return true;
            }
        }
        return false;
    }

    // PATH LOCKING MECHANISM

    // Lock a path with specified mode and ownership
    bool lockPath(const std::string &path, LockMode mode, const std::string &ownerID,
                  const std::string &senderID = "", const std::string &receiverID = "")
    {
        std::string normalizedPath = normalizePath(path);

        // Check if path is already locked
        auto existing = pathLocks.find(normalizedPath);
        if (existing != pathLocks.end() && existing->second.finalized)
        {
            ESP_LOGW(TAG_ROUTER, "Path %s is already finalized and cannot be modified", normalizedPath.c_str());
            return false;
        }

        PathLock lock(normalizedPath, mode, senderID, receiverID);

        // Set appropriate owner based on what's being locked
        if (!senderID.empty())
        {
            lock.senderOwner = ownerID;
        }
        if (!receiverID.empty())
        {
            lock.receiverOwner = ownerID;
        }

        pathLocks[normalizedPath] = lock;

        ESP_LOGI(TAG_ROUTER, "Path locked: %s (mode: %d, owner: %s)",
                 normalizedPath.c_str(), (int)mode, ownerID.c_str());
        return true;
    }

    // Unlock a path (only by owner)
    bool unlockPath(const std::string &path, const std::string &ownerID)
    {
        std::string normalizedPath = normalizePath(path);

        auto it = pathLocks.find(normalizedPath);
        if (it == pathLocks.end())
        {
            return false; // Path not locked
        }

        PathLock &lock = it->second;

        if (lock.finalized)
        {
            ESP_LOGW(TAG_ROUTER, "Cannot unlock finalized path: %s", normalizedPath.c_str());
            return false;
        }

        // Check ownership
        if (lock.senderOwner != ownerID && lock.receiverOwner != ownerID)
        {
            ESP_LOGW(TAG_ROUTER, "Access denied: %s cannot unlock path %s",
                     ownerID.c_str(), normalizedPath.c_str());
            return false;
        }

        pathLocks.erase(it);
        ESP_LOGI(TAG_ROUTER, "Path unlocked: %s by %s", normalizedPath.c_str(), ownerID.c_str());
        return true;
    }

    // Finalize a path lock (makes it permanent)
    bool finalizeLock(const std::string &path, const std::string &ownerID)
    {
        std::string normalizedPath = normalizePath(path);

        auto it = pathLocks.find(normalizedPath);
        if (it == pathLocks.end())
        {
            return false;
        }

        PathLock &lock = it->second;

        // Check ownership
        if (lock.senderOwner != ownerID && lock.receiverOwner != ownerID)
        {
            return false;
        }

        lock.finalized = true;
        ESP_LOGI(TAG_ROUTER, "Path lock finalized: %s", normalizedPath.c_str());
        return true;
    }

    // Get lock status for a path
    PathLock *getLockStatus(const std::string &path)
    {
        std::string normalizedPath = normalizePath(path);
        auto it = pathLocks.find(normalizedPath);
        return (it != pathLocks.end()) ? &it->second : nullptr;
    }

    // ENHANCED MESSAGE SENDING

    // Send message immediately with enhanced parameters
    void sendMessage(const std::string &path, const std::string &data,
                     const std::string &senderID = "", const std::string &receiverID = "")
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        Message message(normalizePath(path), data, senderID, receiverID);
        message.messageId = nextMessageId++;
        processMessageImmediate(message);
        xSemaphoreGiveRecursive(_mutex);
    }

    // Queue message for later processing
    bool queueMessage(const std::string &path, const std::string &data,
                      const std::string &senderID = "", const std::string &receiverID = "",
                      uint32_t priority = 0)
    {
        if (messageQueue.size() >= MAX_QUEUE_SIZE)
        {
            ESP_LOGW(TAG_ROUTER, "Message queue full, dropping oldest message");
            messageQueue.erase(messageQueue.begin());
        }

        Message message(normalizePath(path), data, senderID, receiverID);
        message.messageId = nextMessageId++;
        messageQueue.emplace_back(message, "", priority);
        return true;
    }

    // Process queued messages (respects priority)
    void processQueue()
    {
        xSemaphoreTakeRecursive(_mutex, portMAX_DELAY);
        // Sort by priority (higher priority first)
        std::sort(messageQueue.begin(), messageQueue.end(),
                  [](const QueuedMessage &a, const QueuedMessage &b)
                  {
                      return a.priority > b.priority;
                  });

        while (!messageQueue.empty())
        {
            QueuedMessage queuedMsg = messageQueue.front();
            messageQueue.erase(messageQueue.begin());

            processMessageImmediate(queuedMsg.message, queuedMsg.senderPath);
        }
        xSemaphoreGiveRecursive(_mutex);
    }

    // UTILITY AND DEBUG FUNCTIONS

    // Get listener information by ID
    bool getListenerInfo(uint32_t listenerId, std::string &path, std::string &ownerID, bool &active)
    {
        for (const Listener &listener : listeners)
        {
            if (listener.id == listenerId)
            {
                path = listener.path;
                ownerID = listener.ownerID;
                active = listener.active;
                return true;
            }
        }
        return false;
    }

    // Get all listener IDs for a path
    std::vector<uint32_t> getListenerIds(const std::string &path)
    {
        std::string normalizedPath = normalizePath(path);
        std::vector<uint32_t> ids;

        for (const Listener &listener : listeners)
        {
            if (listener.path == normalizedPath)
            {
                ids.push_back(listener.id);
            }
        }

        return ids;
    }

    // Get queue size
    size_t getQueueSize() const { return messageQueue.size(); }

    // Get listener count
    size_t getListenerCount() const { return listeners.size(); }

    // Get listener count for specific path
    int getListenerCount(const std::string &path) const
    {
        std::string normalizedPath = normalizePath(path);
        int count = 0;

        for (const Listener &listener : listeners)
        {
            if (listener.path == normalizedPath && listener.active)
            {
                count++;
            }
        }

        return count;
    }

    // Clear all listeners
    void clearListeners()
    {
        listeners.clear();
        ESP_LOGI(TAG_ROUTER, "All listeners cleared");
    }

    // Clear message queue
    void clearQueue()
    {
        messageQueue.clear();
        ESP_LOGI(TAG_ROUTER, "Message queue cleared");
    }

    // Clear all path locks
    void clearLocks()
    {
        pathLocks.clear();
        ESP_LOGI(TAG_ROUTER, "All path locks cleared");
    }

    // Debug: Print comprehensive status
    std::string getStatus() const
    {
        std::string status = "";
        status += "Listeners: " + std::to_string(listeners.size()) + " (active: " + std::to_string(getActiveListenerCount()) + ")\n";
        status += "Queue size: " + std::to_string(messageQueue.size()) + "\n";
        status += "Path locks: " + std::to_string(pathLocks.size()) + "\n";
        status += "Next listener ID: " + std::to_string(nextListenerId) + "\n";
        status += "Next message ID: " + std::to_string(nextMessageId) + "\n";
        status += "";

        if(const size_t lockCount = pathLocks.size()) {
            status += "Locks detail:\n";
            for (const auto &pair : pathLocks) {
                const PathLock &lock = pair.second;
                status += "  Path: " + lock.path + ", Mode: " + std::to_string((int)lock.mode) +
                          ", Sender: " + (lock.senderID.empty() ? "N/A" : lock.senderID) +
                          ", Receiver: " + (lock.receiverID.empty() ? "N/A" : lock.receiverID) +
                          ", Finalized: " + (lock.finalized ? "Yes" : "No") + "\n";
            }
        } else {
            status += "No active path locks.\n";
        }

        
        
        // active listener count
        if (listeners.size() > 0) {
            status += "Active Listeners:\n";
            for (const Listener &listener : listeners) {
                status += "\tID: " + std::to_string(listener.id) + "\n";
                status += "\tPath: " + listener.path + "\n";
                status += "\tOwner: " + (listener.ownerID.empty() ? "N/A" : listener.ownerID) + "\n";
                status += "\tActive: " + std::string(listener.active ? "Yes" : "No") + "\n";
                status += "\tCreated At: " + std::to_string(listener.createdAt) + "\n";
                status += "\t---\n";
            }
        } else {
            status += "No active listeners.\n";
        }

        return status;
    }

    void printStatus() const
    {
        ESP_LOGI(TAG_ROUTER, "=== Message Router Status ===\n%s", getStatus().c_str());
    }

private:
    int getActiveListenerCount() const
    {
        int count = 0;
        for (const Listener &listener : listeners)
        {
            if (listener.active)
                count++;
        }
        return count;
    }
};

// Enhanced example usage
/* class MessageRouterExample
{
private:
    MessageRouter router;
    uint32_t tempListenerId;
    uint32_t sensorListenerId;

public:
    void setup()
    {
        ESP_LOGI("Example", "Enhanced Message Router Example Starting...");

        // Add listeners with IDs and owners
        uint32_t rootId = router.addListener("/", [](const MessageRouter::Message &msg, const std::string &path, const std::string &senderID)
                                             { 
                                                 ESP_LOGI("ROOT", "From %s: %s -> %s",
                                                         senderID.c_str(), msg.path.c_str(), msg.data.c_str()); 
                                             }, "system");

        sensorListenerId = router.addListener("/sensors", [](const MessageRouter::Message &msg, const std::string &path, const std::string &senderID)
                                              { 
                                                  ESP_LOGI("SENSORS", "From %s: %s -> %s",
                                                          senderID.c_str(), msg.path.c_str(), msg.data.c_str()); 
                                              }, "sensor-manager");

        tempListenerId = router.addListener("/sensors/temperature", [](const MessageRouter::Message &msg, const std::string &path, const std::string &senderID)
                                            { 
                                                ESP_LOGI("TEMP", "From %s: %s -> %s (ID: %u, Time: %llu)",
                                                        senderID.c_str(), msg.path.c_str(), msg.data.c_str(),
                                                        msg.messageId, msg.timestamp); 
                                            }, "temp-sensor");

        // Demonstrate path locking
        char idStr[16];
        snprintf(idStr, sizeof(idStr), "%u", tempListenerId);
        router.lockPath("/sensors/temperature", MessageRouter::LockMode::STRICT,
                        "temp-sensor", "temp-device-01", std::string(idStr));

        router.printStatus();
    }

    void loop()
    {
        static uint64_t lastMessage = 0;
        static int messageCounter = 0;

        router.processQueue();

        uint64_t now = esp_timer_get_time() / 1000;
        if (now - lastMessage > 4000)
        {
            lastMessage = now;
            messageCounter++;

            char idStr[16];
            snprintf(idStr, sizeof(idStr), "%u", tempListenerId);

            switch (messageCounter % 6)
            {
            case 0:
                ESP_LOGI("Example", "\n--- Sending from temp-device-01 to specific listener ---");
                router.sendMessage("/sensors/temperature", "25.6°C",
                                   "temp-device-01", std::string(idStr));
                break;

            case 1:
                ESP_LOGI("Example", "\n--- Sending from unauthorized sender (should be blocked) ---");
                router.sendMessage("/sensors/temperature", "HACK ATTEMPT",
                                   "malicious-device", std::string(idStr));
                break;

            case 2:
                ESP_LOGI("Example", "\n--- Sending to sensors (bubbling test) ---");
                router.sendMessage("/sensors", "All sensors OK", "sensor-manager");
                break;

            case 3:
                ESP_LOGI("Example", "\n--- Queuing high priority message ---");
                router.queueMessage("/sensors/alert", "HIGH TEMP!", "temp-device-01", "", 10);
                break;

            case 4:
                ESP_LOGI("Example", "\n--- Testing listener deactivation ---");
                router.setListenerActive(sensorListenerId, false);
                router.sendMessage("/sensors", "This should only reach root", "test");
                router.setListenerActive(sensorListenerId, true);
                break;

            case 5:
                ESP_LOGI("Example", "\n--- Listener management test ---");
                std::vector<uint32_t> ids = router.getListenerIds("/sensors");
                ESP_LOGI("Example", "Found %d listeners on /sensors", ids.size());
                for (uint32_t id : ids)
                {
                    ESP_LOGI("Example", "  Listener ID: %u", id);
                }
                break;
            }

            ESP_LOGI("Example", "Queue size: %d", router.getQueueSize());
        }
    }

    void demonstrateLocking()
    {
        ESP_LOGI("Example", "\n=== Path Locking Demonstration ===");

        // Show lock status
        MessageRouter::PathLock *lock = router.getLockStatus("/sensors/temperature");
        if (lock)
        {
            ESP_LOGI("Example", "Path /sensors/temperature is locked (mode: %d)", (int)lock->mode);
        }

        // Try to unlock with wrong owner (should fail)
        bool result = router.unlockPath("/sensors/temperature", "wrong-owner");
        ESP_LOGI("Example", "Unlock attempt by wrong owner: %s", result ? "SUCCESS" : "FAILED");

        // Unlock with correct owner
        result = router.unlockPath("/sensors/temperature", "temp-sensor");
        ESP_LOGI("Example", "Unlock attempt by correct owner: %s", result ? "SUCCESS" : "FAILED");
    }
}; */

#endif // MESSAGE_ROUTER_H