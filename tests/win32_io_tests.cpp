#include "core/win32_io.hpp"

#include "core/unique_handle.hpp"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct PipePair final
    {
        oc::core::UniqueHandle server;
        oc::core::UniqueHandle client;
        std::wstring name;
    };

    [[nodiscard]] std::wstring make_unique_pipe_name(const std::wstring_view suffix)
    {
        wchar_t buffer[128]{};
        const DWORD pid = ::GetCurrentProcessId();
        const unsigned long long tick = ::GetTickCount64();
        (void)swprintf_s(buffer, L"\\\\.\\pipe\\oc_new_%ls_%lu_%llu", suffix.data(), pid, tick);
        return std::wstring(buffer);
    }

    [[nodiscard]] std::expected<PipePair, DWORD> create_connected_named_pipe_pair(
        const std::wstring_view suffix,
        const DWORD server_open_mode,
        const DWORD client_desired_access) noexcept
    {
        PipePair pair{};
        try
        {
            pair.name = make_unique_pipe_name(suffix);
        }
        catch (...)
        {
            return std::unexpected(ERROR_OUTOFMEMORY);
        }

        oc::core::UniqueHandle server(::CreateNamedPipeW(
            pair.name.c_str(),
            server_open_mode,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr));
        if (!server.valid())
        {
            return std::unexpected(::GetLastError());
        }

        oc::core::UniqueHandle connect_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!connect_event.valid())
        {
            return std::unexpected(::GetLastError());
        }

        OVERLAPPED connect_overlapped{};
        connect_overlapped.hEvent = connect_event.get();

        const BOOL connect_ok = ::ConnectNamedPipe(server.get(), &connect_overlapped);
        DWORD connect_error = 0;
        if (connect_ok == FALSE)
        {
            connect_error = ::GetLastError();
            if (connect_error != ERROR_IO_PENDING && connect_error != ERROR_PIPE_CONNECTED)
            {
                return std::unexpected(connect_error);
            }
        }

        // Create the client end after starting the (overlapped) connect.
        oc::core::UniqueHandle client(::CreateFileW(
            pair.name.c_str(),
            client_desired_access,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr));
        if (!client.valid())
        {
            return std::unexpected(::GetLastError());
        }

        if (connect_ok == FALSE && connect_error == ERROR_IO_PENDING)
        {
            const DWORD wait_result = ::WaitForSingleObject(connect_event.get(), 5'000);
            if (wait_result != WAIT_OBJECT_0)
            {
                return std::unexpected(wait_result == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError());
            }

            DWORD ignored = 0;
            if (::GetOverlappedResult(server.get(), &connect_overlapped, &ignored, FALSE) == FALSE)
            {
                return std::unexpected(::GetLastError());
            }
        }

        pair.server = std::move(server);
        pair.client = std::move(client);
        return pair;
    }

    [[nodiscard]] bool test_blocking_file_writer_supports_overlapped_pipes()
    {
        auto pipes = create_connected_named_pipe_pair(
            L"writer",
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            GENERIC_READ);
        if (!pipes)
        {
            fwprintf(stderr, L"[win32 io] failed to create writer pipe pair (error=%lu)\n", pipes.error());
            return false;
        }

        constexpr std::string_view payload = "hello from overlapped writer";
        const auto payload_bytes = std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(payload.data()),
            payload.size());

        oc::core::BlockingFileWriter writer(pipes->server.view());
        auto written = writer.write_all(payload_bytes);
        if (!written)
        {
            fwprintf(stderr, L"[win32 io] writer.write_all failed (error=%lu)\n", written.error());
            return false;
        }
        if (written.value() != payload_bytes.size())
        {
            fwprintf(stderr, L"[win32 io] writer.write_all wrote %zu bytes, expected %zu\n", written.value(), payload_bytes.size());
            return false;
        }

        std::vector<char> captured(payload.size(), '\0');
        size_t total_read = 0;
        while (total_read < captured.size())
        {
            DWORD read = 0;
            if (::ReadFile(
                    pipes->client.get(),
                    captured.data() + total_read,
                    static_cast<DWORD>(captured.size() - total_read),
                    &read,
                    nullptr) == FALSE)
            {
                fwprintf(stderr, L"[win32 io] ReadFile failed reading writer output (error=%lu)\n", ::GetLastError());
                return false;
            }
            if (read == 0)
            {
                break;
            }
            total_read += static_cast<size_t>(read);
        }

        if (total_read != payload.size())
        {
            fwprintf(stderr, L"[win32 io] captured %zu bytes, expected %zu\n", total_read, payload.size());
            return false;
        }

        if (std::string_view(captured.data(), captured.size()) != payload)
        {
            fwprintf(stderr, L"[win32 io] payload mismatch in writer test\n");
            return false;
        }

        return true;
    }

    [[nodiscard]] bool test_blocking_file_reader_supports_overlapped_pipes()
    {
        auto pipes = create_connected_named_pipe_pair(
            L"reader",
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
            GENERIC_WRITE);
        if (!pipes)
        {
            fwprintf(stderr, L"[win32 io] failed to create reader pipe pair (error=%lu)\n", pipes.error());
            return false;
        }

        constexpr std::string_view payload = "hello from overlapped reader";
        DWORD written = 0;
        if (::WriteFile(
                pipes->client.get(),
                payload.data(),
                static_cast<DWORD>(payload.size()),
                &written,
                nullptr) == FALSE)
        {
            fwprintf(stderr, L"[win32 io] WriteFile failed writing reader payload (error=%lu)\n", ::GetLastError());
            return false;
        }
        if (written != payload.size())
        {
            fwprintf(stderr, L"[win32 io] wrote %lu bytes, expected %zu\n", written, payload.size());
            return false;
        }

        std::vector<std::byte> captured(payload.size());
        oc::core::BlockingFileReader reader(pipes->server.view());

        size_t total_read = 0;
        while (total_read < captured.size())
        {
            auto read = reader.read(std::span<std::byte>(captured.data() + total_read, captured.size() - total_read));
            if (!read)
            {
                fwprintf(stderr, L"[win32 io] reader.read failed (error=%lu)\n", read.error());
                return false;
            }
            if (read.value() == 0)
            {
                break;
            }
            total_read += static_cast<size_t>(read.value());
        }

        if (total_read != payload.size())
        {
            fwprintf(stderr, L"[win32 io] captured %zu bytes, expected %zu\n", total_read, payload.size());
            return false;
        }

        if (std::memcmp(captured.data(), payload.data(), payload.size()) != 0)
        {
            fwprintf(stderr, L"[win32 io] payload mismatch in reader test\n");
            return false;
        }

        return true;
    }
}

bool run_win32_io_tests()
{
    if (!test_blocking_file_writer_supports_overlapped_pipes())
    {
        fwprintf(stderr, L"[win32 io] test_blocking_file_writer_supports_overlapped_pipes failed\n");
        return false;
    }

    if (!test_blocking_file_reader_supports_overlapped_pipes())
    {
        fwprintf(stderr, L"[win32 io] test_blocking_file_reader_supports_overlapped_pipes failed\n");
        return false;
    }

    return true;
}
