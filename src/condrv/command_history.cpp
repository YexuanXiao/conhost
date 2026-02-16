#include "condrv/command_history.hpp"

#include <algorithm>
#include <limits>

namespace oc::condrv
{
    [[nodiscard]] bool CommandHistory::app_name_matches(const std::wstring_view other) const noexcept
    {
        if (_app_name.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            other.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            // `CompareStringOrdinal` takes `int` lengths. Extremely large names are not expected.
            return false;
        }

        return ::CompareStringOrdinal(
                   _app_name.data(),
                   static_cast<int>(_app_name.size()),
                   other.data(),
                   static_cast<int>(other.size()),
                   TRUE) == CSTR_EQUAL;
    }

    [[nodiscard]] bool CommandHistory::try_set_app_name(const std::wstring_view app_name) noexcept
    {
        try
        {
            _app_name.assign(app_name);
            return true;
        }
        catch (...)
        {
            _app_name.clear();
            return false;
        }
    }

    void CommandHistory::assign_process(const process_handle_t process_handle) noexcept
    {
        _process_handle = process_handle;
        _allocated = true;
    }

    void CommandHistory::release_process() noexcept
    {
        _allocated = false;
        _process_handle = 0;
    }

    void CommandHistory::clear_commands() noexcept
    {
        _commands.clear();
    }

    void CommandHistory::realloc(const size_t max_commands) noexcept
    {
        _max_commands = max_commands;

        // Match the upstream vector-based semantics: reducing the maximum length truncates from the
        // end (newest commands). This is not ideal, but is the observable behavior today.
        if (_commands.size() > max_commands)
        {
            _commands.resize(max_commands);
        }
    }

    void CommandHistory::add(const std::wstring_view command, const bool suppress_duplicates) noexcept
    {
        if (_max_commands == 0 || command.empty())
        {
            return;
        }

        // The inbox host avoids inserting immediate duplicates.
        if (!_commands.empty() && _commands.back() == command)
        {
            return;
        }

        if (suppress_duplicates)
        {
            const auto it = std::find(_commands.begin(), _commands.end(), command);
            if (it != _commands.end())
            {
                _commands.erase(it);
            }
        }

        if (_commands.size() == _max_commands)
        {
            _commands.erase(_commands.begin());
        }

        try
        {
            _commands.emplace_back(command);
        }
        catch (...)
        {
            // Best-effort: cooked reads can succeed even if history insertion fails.
        }
    }

    void CommandHistoryPool::resize_all(const size_t max_commands) noexcept
    {
        for (auto& entry : _histories)
        {
            entry.realloc(max_commands);
        }
    }

    void CommandHistoryPool::allocate_for_process(
        const std::wstring_view app_name,
        const process_handle_t process_handle,
        const size_t max_histories,
        const size_t default_max_commands) noexcept
    {
        // Find an unallocated buffer with the same app name (MRU for this app).
        auto best_candidate = _histories.end();
        bool same_app = false;

        for (auto it = _histories.begin(); it != _histories.end(); ++it)
        {
            if (it->allocated())
            {
                continue;
            }

            if (it->app_name_matches(app_name))
            {
                best_candidate = it;
                same_app = true;
                break;
            }
        }

        // If there isn't a free buffer for this app name and we still have capacity,
        // allocate a new history entry.
        if (!same_app && _histories.size() < max_histories)
        {
            CommandHistory history{};
            (void)history.try_set_app_name(app_name);
            history.realloc(default_max_commands);
            history.assign_process(process_handle);

            try
            {
                _histories.emplace_front(std::move(history));
            }
            catch (...)
            {
                // Best-effort: failure to allocate history storage should not block CONNECT.
            }
            return;
        }

        // Otherwise, reuse an unallocated entry. Prefer one with an empty command list.
        if (best_candidate == _histories.end())
        {
            for (auto it = _histories.begin(); it != _histories.end(); ++it)
            {
                if (it->allocated())
                {
                    continue;
                }

                if (it->commands().empty() || best_candidate == _histories.end() || !best_candidate->commands().empty())
                {
                    best_candidate = it;
                }
            }
        }

        if (best_candidate == _histories.end())
        {
            return;
        }

        if (!same_app)
        {
            best_candidate->clear_commands();
            if (!best_candidate->try_set_app_name(app_name))
            {
                return;
            }
        }

        best_candidate->assign_process(process_handle);
        _histories.splice(_histories.begin(), _histories, best_candidate);
    }

    void CommandHistoryPool::free_for_process(const process_handle_t process_handle) noexcept
    {
        auto* history = find_by_process(process_handle);
        if (history != nullptr)
        {
            history->release_process();
        }
    }

    [[nodiscard]] CommandHistory* CommandHistoryPool::find_by_process(const process_handle_t process_handle) noexcept
    {
        for (auto& entry : _histories)
        {
            if (entry.allocated() && entry.process_handle() == process_handle)
            {
                return &entry;
            }
        }

        return nullptr;
    }

    [[nodiscard]] const CommandHistory* CommandHistoryPool::find_by_process(const process_handle_t process_handle) const noexcept
    {
        for (const auto& entry : _histories)
        {
            if (entry.allocated() && entry.process_handle() == process_handle)
            {
                return &entry;
            }
        }

        return nullptr;
    }

    [[nodiscard]] CommandHistory* CommandHistoryPool::find_by_exe(const std::wstring_view exe_name) noexcept
    {
        for (auto& entry : _histories)
        {
            if (entry.allocated() && entry.app_name_matches(exe_name))
            {
                return &entry;
            }
        }

        return nullptr;
    }

    [[nodiscard]] const CommandHistory* CommandHistoryPool::find_by_exe(const std::wstring_view exe_name) const noexcept
    {
        for (const auto& entry : _histories)
        {
            if (entry.allocated() && entry.app_name_matches(exe_name))
            {
                return &entry;
            }
        }

        return nullptr;
    }

    void CommandHistoryPool::expunge_by_exe(const std::wstring_view exe_name) noexcept
    {
        auto* history = find_by_exe(exe_name);
        if (history != nullptr)
        {
            history->clear_commands();
        }
    }

    void CommandHistoryPool::set_number_of_commands_by_exe(const std::wstring_view exe_name, const size_t max_commands) noexcept
    {
        for (auto it = _histories.begin(); it != _histories.end(); ++it)
        {
            if (it->allocated() && it->app_name_matches(exe_name))
            {
                it->realloc(max_commands);
                _histories.splice(_histories.begin(), _histories, it);
                return;
            }
        }
    }
}

