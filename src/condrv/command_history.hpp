#pragma once

// A minimal, conhost-style command history pool.
//
// The inbox host keeps a small LRU pool of per-executable command histories.
// Each connecting process is associated with one history buffer, identified by
// the application's "AppName" reported in the CONNECT message. Selected L3 APIs
// allow callers to query and mutate this history by EXE name.
//
// This module intentionally implements only the subset needed for:
// - recording cooked `ReadConsole` line input on Enter
// - `ConsolepExpungeCommandHistory`
// - `ConsolepSetNumberOfCommands`
// - `ConsolepGetCommandHistoryLength`
// - `ConsolepGetCommandHistory`
//
// It does not implement interactive history navigation (VK_UP/DOWN, F7, etc.).

#include "core/assert.hpp"

#include <Windows.h>

#include <list>
#include <string>
#include <string_view>
#include <vector>

namespace oc::condrv
{
    class CommandHistory final
    {
    public:
        using process_handle_t = ULONG_PTR;

        [[nodiscard]] bool allocated() const noexcept
        {
            return _allocated;
        }

        [[nodiscard]] process_handle_t process_handle() const noexcept
        {
            return _process_handle;
        }

        [[nodiscard]] std::wstring_view app_name() const noexcept
        {
            return _app_name;
        }

        [[nodiscard]] size_t max_commands() const noexcept
        {
            return _max_commands;
        }

        [[nodiscard]] const std::vector<std::wstring>& commands() const noexcept
        {
            return _commands;
        }

        [[nodiscard]] bool app_name_matches(std::wstring_view other) const noexcept;

        [[nodiscard]] bool try_set_app_name(std::wstring_view app_name) noexcept;

        void assign_process(process_handle_t process_handle) noexcept;
        void release_process() noexcept;

        void clear_commands() noexcept;
        void realloc(size_t max_commands) noexcept;

        void add(std::wstring_view command, bool suppress_duplicates) noexcept;

    private:
        std::vector<std::wstring> _commands;
        size_t _max_commands{};
        std::wstring _app_name;
        process_handle_t _process_handle{};
        bool _allocated{};
    };

    class CommandHistoryPool final
    {
    public:
        using process_handle_t = CommandHistory::process_handle_t;

        void resize_all(size_t max_commands) noexcept;

        void allocate_for_process(
            std::wstring_view app_name,
            process_handle_t process_handle,
            size_t max_histories,
            size_t default_max_commands) noexcept;

        void free_for_process(process_handle_t process_handle) noexcept;

        [[nodiscard]] CommandHistory* find_by_process(process_handle_t process_handle) noexcept;
        [[nodiscard]] const CommandHistory* find_by_process(process_handle_t process_handle) const noexcept;

        [[nodiscard]] CommandHistory* find_by_exe(std::wstring_view exe_name) noexcept;
        [[nodiscard]] const CommandHistory* find_by_exe(std::wstring_view exe_name) const noexcept;

        void expunge_by_exe(std::wstring_view exe_name) noexcept;
        void set_number_of_commands_by_exe(std::wstring_view exe_name, size_t max_commands) noexcept;

        [[nodiscard]] size_t count() const noexcept
        {
            return _histories.size();
        }

    private:
        std::list<CommandHistory> _histories;
    };
}

