#pragma once
#include "../window_manager/window_manager.h"
#include "../globals.h"
#include <MinHook.h>
#include <imgui.h>
#include <vector>
#include <string>
#include <algorithm>
#include <ctime>

// Game's internal string type (SSO - Small String Optimization)
struct GameString {
    union {
        char buf[16];       // Inline buffer (used when capacity <= 0x10)
        char* ptr;          // Heap pointer (used when capacity > 0x10)
    };
    uint32_t size;          // String length
    uint32_t capacity;      // Allocated capacity

    const char* get() const {
        return (capacity > 0x10) ? ptr : buf;
    }
};

// Console command entry as stored in the hash table
struct ConsoleCommand {
    char _pad[0xA8]; // Reserve full struct size

    // Accessor methods using offsets
    void*& func_ptr() { return *(void**)((char*)this + 0x00); }
    GameString& name() { return *(GameString*)((char*)this + 0x08); }
    GameString& usage() { return *(GameString*)((char*)this + 0x48); }
    uint32_t& hash() { return *(uint32_t*)((char*)this + 0x88); }
    uint8_t& min_args() { return *(uint8_t*)((char*)this + 0x98); }
    uint8_t& max_args() { return *(uint8_t*)((char*)this + 0x99); }
    ConsoleCommand*& next() { return *(ConsoleCommand**)((char*)this + 0xA0); }
};

// Log entry for command execution attempts
struct CommandLogEntry {
    std::time_t timestamp;
    std::string command;
    int argc;
    std::string result; // "Found", "Not Found", "Cheat Blocked", "Invalid Args"
};

class console_debug : public window {
    WINDOW_DEFINE(console_debug, "Debug", "Console Debug", true);

private:
    // Original function pointers
    static inline uint64_t (*ConsoleCommandHandler_orig)(int argc, uint64_t* argv) = nullptr;
    static inline void (*ConsoleCheatCommandCheck_orig)(int64_t* param_1, int64_t* param_2) = nullptr;

    // State
    static inline bool g_bypassCheatCheck = false;
    static inline std::vector<CommandLogEntry> g_commandLog;
    static inline bool g_autoScrollLog = true;
    static inline ImGuiTextFilter g_commandFilter;
    static inline int g_maxLogEntries = 1000;

    // Cached command list
    std::vector<ConsoleCommand*> m_cachedCommands;
    bool m_needsRefresh = true;

    // RVA offsets (from Ghidra analysis, image base 0x7ff64ffe0000)
    static constexpr uint64_t RVA_ConsoleCommandHandler = 0x7E6160;
    static constexpr uint64_t RVA_ConsoleCheatCommandCheck = 0xC70A40;
    static constexpr uint64_t RVA_ConsoleCheatCommandExecute = 0xC70840;
    static constexpr uint64_t RVA_CommandHashTable = 0x181E600;
    static constexpr uint64_t RVA_CommandCount = 0x181EE00;

    // Hook functions
    static uint64_t ConsoleCommandHandler_hook(int argc, uint64_t* argv) {
        // Call original first
        uint64_t result = 0;
        if (ConsoleCommandHandler_orig) {
            result = ConsoleCommandHandler_orig(argc, argv);
        }

        // Log the command attempt (after original call)
        CommandLogEntry entry;
        entry.timestamp = std::time(nullptr);
        entry.argc = argc;

        // Safely read command name
        if (argv && *argv) {
            const char* cmd_name = (const char*)*argv;
            entry.command = cmd_name ? cmd_name : "<null>";
        } else {
            entry.command = "<null argv>";
        }

        entry.result = (result == 0) ? "Not Found/Failed" : "Executed";

        // Add to log (with size limit)
        g_commandLog.push_back(entry);
        if (g_commandLog.size() > (size_t)g_maxLogEntries) {
            g_commandLog.erase(g_commandLog.begin());
        }

        return result;
    }

    static void ConsoleCheatCommandCheck_hook(int64_t* param_1, int64_t* param_2) {
        if (g_bypassCheatCheck) {
            // Skip connection check, go straight to execution
            auto execute = (void(*)(int64_t*, int64_t*))(globals::gameBase + RVA_ConsoleCheatCommandExecute);
            execute(param_1 - 0x54, param_2);

            // Log bypass event
            CommandLogEntry entry;
            entry.timestamp = std::time(nullptr);
            entry.command = "<cheat command>";
            entry.argc = 0;
            entry.result = "Bypass Enabled";

            g_commandLog.push_back(entry);
            if (g_commandLog.size() > (size_t)g_maxLogEntries) {
                g_commandLog.erase(g_commandLog.begin());
            }
            return;
        }

        // Normal behavior
        if (ConsoleCheatCommandCheck_orig) {
            ConsoleCheatCommandCheck_orig(param_1, param_2);
        }
    }

    void refreshCommands() {
        m_cachedCommands.clear();

        // Safety check - ensure game base is initialized
        if (globals::gameBase == 0) {
            m_needsRefresh = false;
            return;
        }

        // Read hash table (256 buckets)
        ConsoleCommand** hashTable = (ConsoleCommand**)(globals::gameBase + RVA_CommandHashTable);

        // Validate hash table pointer
        if (IsBadReadPtr(hashTable, sizeof(void*) * 256)) {
            m_needsRefresh = false;
            return;
        }

        for (int bucket = 0; bucket < 256; bucket++) {
            ConsoleCommand* cmd = hashTable[bucket];

            // Walk linked list for this bucket
            while (cmd != nullptr) {
                // Validate pointer before dereferencing
                if (IsBadReadPtr(cmd, sizeof(ConsoleCommand))) {
                    break;
                }

                m_cachedCommands.push_back(cmd);

                // Get next in chain
                cmd = cmd->next();
            }
        }

        m_needsRefresh = false;
    }

public:
    inline void preStartInitialize() override {
        // Hook ConsoleCommandHandler
        MH_VERIFY(MH_CreateHook(
            (PVOID)(globals::gameBase + RVA_ConsoleCommandHandler),
            ConsoleCommandHandler_hook,
            (PVOID*)&ConsoleCommandHandler_orig));
        MH_VERIFY(MH_EnableHook((PVOID)(globals::gameBase + RVA_ConsoleCommandHandler)));

        // Hook ConsoleCheatCommandCheck
        MH_VERIFY(MH_CreateHook(
            (PVOID)(globals::gameBase + RVA_ConsoleCheatCommandCheck),
            ConsoleCheatCommandCheck_hook,
            (PVOID*)&ConsoleCheatCommandCheck_orig));
        MH_VERIFY(MH_EnableHook((PVOID)(globals::gameBase + RVA_ConsoleCheatCommandCheck)));
    }

    inline void render() override {
        if (open_window()) {
            // Controls section
            if (ImGui::CollapsingHeader("Controls", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::Checkbox("Bypass Cheat Command Check", &g_bypassCheatCheck);
                ImGui::SameLine();
                if (ImGui::Button("?")) {
                    ImGui::SetTooltip("Allows cheat commands to execute without server connection.\n"
                                     "WARNING: Commands may still have no effect without a server.");
                }

                if (ImGui::Button("Refresh Commands")) {
                    m_needsRefresh = true;
                }
                ImGui::SameLine();
                ImGui::Text("Registered Commands: %d", (int)m_cachedCommands.size());

                ImGui::Checkbox("Auto-scroll Log", &g_autoScrollLog);
                ImGui::SameLine();
                if (ImGui::Button("Clear Log")) {
                    g_commandLog.clear();
                }
            }

            ImGui::Separator();

            // Registered Commands Table
            if (ImGui::CollapsingHeader("Registered Commands", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Only refresh if game is fully initialized
                if (m_needsRefresh && globals::gameBase != 0) {
                    refreshCommands();
                }

                g_commandFilter.Draw("Filter");

                if (ImGui::BeginTable("CommandsTable", 7,
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable |
                    ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersH |
                    ImGuiTableFlags_HighlightHoveredColumn | ImGuiTableFlags_RowBg,
                    ImVec2(0, 300))) {

                    ImGui::TableSetupColumn("Bucket", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                    ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("Usage", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    for (auto* cmd : m_cachedCommands) {
                        const char* name = cmd->name().get();
                        if (!name || name[0] == '\0') continue;

                        // Apply filter
                        if (!g_commandFilter.PassFilter(name)) continue;

                        ImGui::TableNextRow();

                        // Bucket (hash low byte)
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", cmd->hash() & 0xFF);

                        // Name
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", name);

                        // Hash
                        ImGui::TableNextColumn();
                        ImGui::Text("%08X", cmd->hash());

                        // Min Args
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", cmd->min_args());

                        // Max Args
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", cmd->max_args());

                        // Usage
                        ImGui::TableNextColumn();
                        const char* usage = cmd->usage().get();
                        ImGui::TextWrapped("%s", usage ? usage : "");

                        // Function Address (as RVA)
                        ImGui::TableNextColumn();
                        uint64_t rva = (uint64_t)cmd->func_ptr() - globals::gameBase;
                        ImGui::Text("%08llX", rva);
                    }

                    ImGui::EndTable();
                }
            }

            ImGui::Separator();

            // Command Execution Log
            if (ImGui::CollapsingHeader("Command Execution Log", ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("LogTable", 4,
                    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                    ImGuiTableFlags_BordersH | ImGuiTableFlags_RowBg,
                    ImVec2(0, 200))) {

                    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40.0f);
                    ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Args", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    for (size_t i = 0; i < g_commandLog.size(); i++) {
                        const auto& entry = g_commandLog[i];

                        ImGui::TableNextRow();

                        // Index
                        ImGui::TableNextColumn();
                        ImGui::Text("%zu", i);

                        // Command
                        ImGui::TableNextColumn();
                        ImGui::Text("%s", entry.command.c_str());

                        // Args
                        ImGui::TableNextColumn();
                        ImGui::Text("%d", entry.argc);

                        // Result
                        ImGui::TableNextColumn();
                        if (entry.result == "Executed") {
                            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", entry.result.c_str());
                        } else if (entry.result == "Bypass Enabled") {
                            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "%s", entry.result.c_str());
                        } else {
                            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "%s", entry.result.c_str());
                        }
                    }

                    if (g_autoScrollLog && ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
                        ImGui::SetScrollHereY(1.0f);
                    }

                    ImGui::EndTable();
                }
            }
        }
        ImGui::End();
    }
};

WINDOW_REGISTER(console_debug);
