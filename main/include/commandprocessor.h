/**
 * @file command_processor.h
 * @brief Command Processor module for FabLink firmware (ESP-IDF)
 *
 * This module implements a lightweight, extensible command-line interface (CLI)
 * for the FabLink firmware. It allows text-based commands—received via serial,
 * web, or network terminals—to be parsed, interpreted, and executed dynamically.
 *
 * The Command Processor supports:
 *  - Command registration and deregistration at runtime
 *  - Argument, flag, and option parsing (e.g., `--key=value`, `-f`, `"quoted args"`)
 *  - Safe command execution with exception handling
 *  - Integration with the MessageRouter for inter-module communication
 *
 * Built-in commands include:
 *  - `help`   — Show command list or detailed help
 *  - `send`   — Send messages via the MessageRouter
 *  - `reboot` — Restart the ESP device
 *  - `clear`  — Clear terminal output
 *
 * This module forms the foundation of FabLink’s interactive control layer,
 * enabling system commands, debugging, and automation directly through
 * human-readable interfaces.
 *
 * @note Designed for use with the ESP-IDF framework.
 * @see MessageRouter
 * @see esp_log.h
 * @see esp_system.h
 * 
 * @author Hassaan Maqsood
 */

#ifndef COMMAND_PROCESSOR_H
#define COMMAND_PROCESSOR_H

#include <string>
#include <vector>
#include <map>
#include <functional>
#include "esp_system.h"
#include "esp_log.h"
#include "messagerouter.h"

static const char *TAG_CMD = "CMDProcessor";

class MessageRouter;

// Structure to hold parsed command data
struct ParsedCommand
{
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> flags;
    std::map<std::string, std::string> options;
};

// Structure to hold command registration data
struct CommandInfo
{
    std::function<std::string(const ParsedCommand &)> handler;
    std::string description;
    std::string usage;

    CommandInfo() {}
    CommandInfo(std::function<std::string(const ParsedCommand &)> h, 
                const std::string &desc, 
                const std::string &u)
        : handler(h), description(desc), usage(u) {}
};

class CommandProcessor
{
private:
    MessageRouter *_router = nullptr;
    std::map<std::string, CommandInfo> _commands;
    std::string _lastResult;

    // Core parsing functions
    ParsedCommand parseCommand(const std::string &input)
    {
        ParsedCommand cmd;
        std::vector<std::string> tokens = tokenize(input);

        if (tokens.empty())
            return cmd;

        cmd.command = tokens[0];
        // Convert to lowercase
        std::transform(cmd.command.begin(), cmd.command.end(), 
                      cmd.command.begin(), ::tolower);

        for (size_t i = 1; i < tokens.size(); i++)
        {
            std::string token = tokens[i];

            if (token.rfind("--", 0) == 0)
            {
                // Long option: --key=value or --key value
                handleLongOption(token, tokens, i, cmd);
            }
            else if (token.rfind("-", 0) == 0 && token.length() > 1)
            {
                // Short flag: -f or -abc (multiple flags)
                handleShortFlags(token, cmd);
            }
            else
            {
                // Regular argument
                cmd.args.push_back(token);
            }
        }

        return cmd;
    }

    std::vector<std::string> tokenize(const std::string &input)
    {
        std::vector<std::string> tokens;
        std::string current = "";
        bool inQuotes = false;
        bool escaped = false;

        for (size_t i = 0; i < input.length(); i++)
        {
            char c = input[i];

            if (escaped)
            {
                current += c;
                escaped = false;
                continue;
            }

            if (c == '\\')
            {
                escaped = true;
                continue;
            }

            if (c == '"' || c == '\'')
            {
                inQuotes = !inQuotes;
                continue;
            }

            if (!inQuotes && (c == ' ' || c == '\t'))
            {
                if (current.length() > 0)
                {
                    tokens.push_back(current);
                    current = "";
                }
            }
            else
            {
                current += c;
            }
        }

        if (current.length() > 0)
        {
            tokens.push_back(current);
        }

        return tokens;
    }

    void handleLongOption(const std::string &token, 
                         const std::vector<std::string> &tokens, 
                         size_t &i, 
                         ParsedCommand &cmd)
    {
        std::string option = token.substr(2); // Remove --

        size_t equalPos = option.find('=');
        if (equalPos != std::string::npos)
        {
            // --key=value format
            std::string key = option.substr(0, equalPos);
            std::string value = option.substr(equalPos + 1);
            cmd.options[key] = value;
        }
        else
        {
            // --key value format (if next token is not a flag/option)
            if (i + 1 < tokens.size() && tokens[i + 1].rfind("-", 0) != 0)
            {
                cmd.options[option] = tokens[++i];
            }
            else
            {
                // Boolean flag
                cmd.flags[option] = "true";
            }
        }
    }

    void handleShortFlags(const std::string &token, ParsedCommand &cmd)
    {
        // Handle multiple short flags like -abc
        for (size_t j = 1; j < token.length(); j++)
        {
            std::string flag(1, token[j]);
            cmd.flags[flag] = "true";
        }
    }

    // Built-in command handlers
    std::string handleHelp(const ParsedCommand &cmd)
    {
        std::string result = "Available Commands:\n";

        if (!cmd.args.empty())
        {
            // Help for specific command
            std::string cmdName = cmd.args[0];
            auto it = _commands.find(cmdName);
            if (it != _commands.end())
            {
                const CommandInfo &info = it->second;
                result += "\nCommand: " + cmdName + "\n";
                result += "Description: " + info.description + "\n";
                result += "Usage: " + info.usage + "\n";
            }
            else
            {
                result += "Command '" + cmdName + "' not found.\n";
            }
        }
        else
        {
            // List all commands
            for (const auto &pair : _commands)
            {
                result += "  " + pair.first;
                if (pair.second.description.length() > 0)
                {
                    result += " - " + pair.second.description;
                }
                result += "\n";
            }
            result += "\nUse 'help <command>' for detailed help on a specific command.\n";
        }

        return result;
    }

    std::string handleStatus(const ParsedCommand &cmd)
    {
        std::string result = "=== System Status ===\n";

        bool verbose = cmd.flags.find("v") != cmd.flags.end() ||
                       cmd.flags.find("verbose") != cmd.flags.end();

        // System Info
        result += "Free Heap: " + std::to_string(esp_get_free_heap_size() / 1024) + " KB\n";
        result += "Min Free Heap: " + std::to_string(esp_get_minimum_free_heap_size() / 1024) + " KB\n";
        result += "Uptime: " + std::to_string(esp_timer_get_time() / 1000000) + " seconds\n";

        if (_router != nullptr)
        {
            result += "Router: Connected\n";
        }
        else
        {
            result += "Router: Not Connected\n";
        }

        if (verbose)
        {
            result += "Registered Commands: " + std::to_string(_commands.size()) + "\n";
            result += "Last Command Result: " + 
                     std::string(_lastResult.length() > 0 ? "Available" : "None") + "\n";
        }

        return result;
    }

    std::string handleSend(const ParsedCommand &cmd)
    {
        if (cmd.args.empty())
        {
            return "Usage: send <message> [--path=/com] [--id=sys]\n";
        }

        if (_router == nullptr)
        {
            return "Error: Router not connected\n";
        }

        std::string message = "";
        for (size_t i = 0; i < cmd.args.size(); i++)
        {
            if (i > 0)
                message += " ";
            message += cmd.args[i];
        }

        std::string path = "/com";
        std::string id = "sys";

        auto pathIt = cmd.options.find("path");
        if (pathIt != cmd.options.end())
        {
            path = pathIt->second;
        }

        auto idIt = cmd.options.find("id");
        if (idIt != cmd.options.end())
        {
            id = idIt->second;
        }

        _router->sendMessage(path.c_str(), (message + "\n").c_str(), id.c_str(), "");
        return "Message sent to " + path + " from " + id + "\n";
    }

    std::string handleReboot(const ParsedCommand &cmd)
    {
        ESP_LOGI(TAG_CMD, "Rebooting system...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return "Rebooting...\n"; // Won't actually return
    }

    std::string handleClear(const ParsedCommand &cmd)
    {
        return "\033[2J\033[H"; // ANSI clear screen
    }

public:
    void begin(MessageRouter *router)
    {
        _router = router;
        registerBuiltinCommands();
        ESP_LOGI(TAG_CMD, "Command processor initialized");
    }

    void registerBuiltinCommands()
    {
        registerCommand("help", 
            [this](const ParsedCommand &cmd) { return handleHelp(cmd); },
            "Show help information", 
            "help [command]");

        /* registerCommand("status", 
            [this](const ParsedCommand &cmd) { return handleStatus(cmd); },
            "Show system status", 
            "status [-v|--verbose]"); */

        registerCommand("send", 
            [this](const ParsedCommand &cmd) { return handleSend(cmd); },
            "Send message via router", 
            "send <message> [--path=/com] [--id=sys]");

        registerCommand("reboot", 
            [this](const ParsedCommand &cmd) { return handleReboot(cmd); },
            "Restart device", 
            "reboot");

        registerCommand("clear", 
            [this](const ParsedCommand &cmd) { return handleClear(cmd); },
            "Clear terminal screen", 
            "clear");
    }

    // Public API for command registration
    bool registerCommand(const std::string &name,
                        std::function<std::string(const ParsedCommand &)> handler,
                        const std::string &description = "",
                        const std::string &usage = "")
    {
        if (name.empty())
            return false;

        _commands[name] = CommandInfo(handler, description, usage);
        return true;
    }

    bool unregisterCommand(const std::string &name)
    {
        return _commands.erase(name) > 0;
    }

    bool isCommandRegistered(const std::string &name) const
    {
        return _commands.find(name) != _commands.end();
    }

    std::vector<std::string> getRegisteredCommands() const
    {
        std::vector<std::string> commands;
        for (const auto &pair : _commands)
        {
            commands.push_back(pair.first);
        }
        return commands;
    }

    // Main processing function
    std::string processCommand(const std::string &input)
    {
        if (input.empty())
        {
            return "";
        }

        ParsedCommand cmd = parseCommand(input);

        if (cmd.command.empty())
        {
            return "";
        }

        std::string result;

        // Check if command is registered
        auto it = _commands.find(cmd.command);
        if (it != _commands.end())
        {
            const CommandInfo &info = it->second;
            try
            {
                result = info.handler(cmd);
            }
            catch (...)
            {
                result = "Error: Command execution failed\n";
                ESP_LOGE(TAG_CMD, "Command execution failed: %s", cmd.command.c_str());
            }
        }
        else
        {
            result = "Unknown command: '" + cmd.command + 
                    "'. Type 'help' for available commands.\n";
        }

        _lastResult = result;
        return result;
    }

    // Utility functions
    std::string getLastResult() const
    {
        return _lastResult;
    }

    void clearLastResult()
    {
        _lastResult = "";
    }
};

#endif // COMMAND_PROCESSOR_H