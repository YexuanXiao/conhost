#pragma once

// Blocking Win32 ReadFile/WriteFile helpers that adapt to overlapped handles.
//
// Why this exists:
// - The default-terminal handoff interface (ITerminalHandoff3) allows the terminal
//   to choose whether the returned pipes use overlapped I/O.
// - If a handle was opened with FILE_FLAG_OVERLAPPED, calling ReadFile/WriteFile
//   with `lpOverlapped == nullptr` fails with ERROR_INVALID_PARAMETER.
// - The rest of openconsole_new is written in a mostly synchronous style. This
//   module provides small RAII helpers that keep that style while remaining
//   compatible with overlapped-only handles.
//
// Design notes:
// - These helpers perform "blocking overlapped I/O": issue the I/O with an
//   OVERLAPPED structure, then wait using GetOverlappedResult(..., TRUE).
// - Cancellation is handled by owners via CancelIoEx(handle, nullptr) or by
//   canceling a specific OVERLAPPED pointer. These helpers do not create threads.

#include "core/handle_view.hpp"
#include "core/unique_handle.hpp"

#include <Windows.h>

#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <span>

namespace oc::core
{
    namespace detail
    {
        enum class IoMode : unsigned char
        {
            unknown,
            synchronous,
            overlapped,
        };

        class OverlappedEvent final
        {
        public:
            OverlappedEvent() noexcept = default;

            explicit OverlappedEvent(UniqueHandle event) noexcept :
                _event(std::move(event))
            {
                reset();
            }

            ~OverlappedEvent() noexcept = default;

            OverlappedEvent(const OverlappedEvent&) = delete;
            OverlappedEvent& operator=(const OverlappedEvent&) = delete;

            OverlappedEvent(OverlappedEvent&& other) noexcept :
                _event(std::move(other._event))
            {
                reset();
            }

            OverlappedEvent& operator=(OverlappedEvent&& other) noexcept
            {
                if (this != &other)
                {
                    _event = std::move(other._event);
                    reset();
                }
                return *this;
            }

            [[nodiscard]] static std::expected<OverlappedEvent, DWORD> create() noexcept
            {
                UniqueHandle event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
                if (!event.valid())
                {
                    return std::unexpected(::GetLastError());
                }

                return OverlappedEvent(std::move(event));
            }

            void reset() noexcept
            {
                _overlapped = OVERLAPPED{};
                _overlapped.hEvent = _event.get();
                if (_event.valid())
                {
                    (void)::ResetEvent(_event.get());
                }
            }

            [[nodiscard]] OVERLAPPED* overlapped() noexcept
            {
                return &_overlapped;
            }

        private:
            UniqueHandle _event;
            OVERLAPPED _overlapped{};
        };
    }

    class BlockingFileReader final
    {
    public:
        BlockingFileReader() noexcept = default;

        explicit BlockingFileReader(const HandleView handle) noexcept :
            _handle(handle)
        {
        }

        [[nodiscard]] std::expected<DWORD, DWORD> read(const std::span<std::byte> dest) noexcept
        {
            if (!_handle)
            {
                return std::unexpected(ERROR_INVALID_HANDLE);
            }

            if (dest.empty())
            {
                return DWORD{ 0 };
            }

            if (dest.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
            {
                return std::unexpected(ERROR_INVALID_PARAMETER);
            }

            const DWORD to_read = static_cast<DWORD>(dest.size());
            DWORD read = 0;

            for (;;)
            {
                if (_mode != detail::IoMode::overlapped)
                {
                    if (::ReadFile(_handle.get(), dest.data(), to_read, &read, nullptr) != FALSE)
                    {
                        _mode = detail::IoMode::synchronous;
                        return read;
                    }

                    const DWORD error = ::GetLastError();
                    if (error != ERROR_INVALID_PARAMETER)
                    {
                        return std::unexpected(error);
                    }

                    _mode = detail::IoMode::overlapped;
                }

                if (!_overlapped.has_value())
                {
                    auto created = detail::OverlappedEvent::create();
                    if (!created)
                    {
                        return std::unexpected(created.error());
                    }

                    _overlapped.emplace(std::move(created.value()));
                }

                _overlapped->reset();

                if (::ReadFile(_handle.get(), dest.data(), to_read, &read, _overlapped->overlapped()) != FALSE)
                {
                    return read;
                }

                DWORD error = ::GetLastError();
                if (error != ERROR_IO_PENDING)
                {
                    return std::unexpected(error);
                }

                if (::GetOverlappedResult(_handle.get(), _overlapped->overlapped(), &read, TRUE) == FALSE)
                {
                    return std::unexpected(::GetLastError());
                }

                return read;
            }
        }

        void reset_handle(const HandleView handle) noexcept
        {
            _handle = handle;
            _mode = detail::IoMode::unknown;
            _overlapped.reset();
        }

    private:
        HandleView _handle{};
        detail::IoMode _mode{ detail::IoMode::unknown };
        std::optional<detail::OverlappedEvent> _overlapped;
    };

    class BlockingFileWriter final
    {
    public:
        BlockingFileWriter() noexcept = default;

        explicit BlockingFileWriter(const HandleView handle) noexcept :
            _handle(handle)
        {
        }

        [[nodiscard]] std::expected<DWORD, DWORD> write(const std::span<const std::byte> bytes) noexcept
        {
            if (!_handle)
            {
                return std::unexpected(ERROR_INVALID_HANDLE);
            }

            if (bytes.empty())
            {
                return DWORD{ 0 };
            }

            if (bytes.size() > static_cast<size_t>(std::numeric_limits<DWORD>::max()))
            {
                return std::unexpected(ERROR_INVALID_PARAMETER);
            }

            const DWORD to_write = static_cast<DWORD>(bytes.size());
            DWORD written = 0;

            for (;;)
            {
                if (_mode != detail::IoMode::overlapped)
                {
                    if (::WriteFile(_handle.get(), bytes.data(), to_write, &written, nullptr) != FALSE)
                    {
                        _mode = detail::IoMode::synchronous;
                        return written;
                    }

                    const DWORD error = ::GetLastError();
                    if (error != ERROR_INVALID_PARAMETER)
                    {
                        return std::unexpected(error);
                    }

                    _mode = detail::IoMode::overlapped;
                }

                if (!_overlapped.has_value())
                {
                    auto created = detail::OverlappedEvent::create();
                    if (!created)
                    {
                        return std::unexpected(created.error());
                    }

                    _overlapped.emplace(std::move(created.value()));
                }

                _overlapped->reset();

                if (::WriteFile(_handle.get(), bytes.data(), to_write, &written, _overlapped->overlapped()) != FALSE)
                {
                    return written;
                }

                DWORD error = ::GetLastError();
                if (error != ERROR_IO_PENDING)
                {
                    return std::unexpected(error);
                }

                if (::GetOverlappedResult(_handle.get(), _overlapped->overlapped(), &written, TRUE) == FALSE)
                {
                    return std::unexpected(::GetLastError());
                }

                return written;
            }
        }

        [[nodiscard]] std::expected<size_t, DWORD> write_all(const std::span<const std::byte> bytes) noexcept
        {
            size_t total_written = 0;
            while (total_written < bytes.size())
            {
                const size_t remaining = bytes.size() - total_written;
                const size_t chunk_size = remaining > static_cast<size_t>(std::numeric_limits<DWORD>::max())
                    ? static_cast<size_t>(std::numeric_limits<DWORD>::max())
                    : remaining;

                const auto chunk = bytes.subspan(total_written, chunk_size);
                auto written = write(chunk);
                if (!written)
                {
                    return std::unexpected(written.error());
                }

                const size_t advanced = static_cast<size_t>(written.value());
                total_written += advanced;
                if (advanced == 0)
                {
                    break;
                }
            }

            return total_written;
        }

        void reset_handle(const HandleView handle) noexcept
        {
            _handle = handle;
            _mode = detail::IoMode::unknown;
            _overlapped.reset();
        }

    private:
        HandleView _handle{};
        detail::IoMode _mode{ detail::IoMode::unknown };
        std::optional<detail::OverlappedEvent> _overlapped;
    };
}

