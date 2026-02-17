#pragma once

// A minimal ConDrv server-mode dispatcher and IO loop.
//
// The inbox conhost uses a dedicated IO thread that blocks on IOCTL_CONDRV_READ_IO,
// dispatches the resulting packet, and completes it back to the driver. This module
// provides the beginning of that behavior so `--server` startup can be serviced
// incrementally without taking a dependency on the full upstream console model.
//
// Threading model:
// - The ConDrv server loop is single-threaded and is driven by
//   `IOCTL_CONDRV_READ_IO` packets.
// - Helper threads are used only to bridge non-waitable or blocking resources
//   into explicit cancellation points (for example, monitoring host input pipes
//   and signal handles). No C++ `<thread>` is used; the implementation relies
//   on Win32 threads and strict RAII handle wrappers.
//
// Reply-pending ("CONSOLE_STATUS_WAIT") behavior:
// - Input-dependent requests must not block the server loop.
// - When an operation cannot make progress yet and waiting is allowed, the
//   request is retained and retried later when input arrives.
// - See `new/docs/design/condrv_reply_pending_wait_queue.md`.
//
// Current scope (incremental):
// - CONNECT / DISCONNECT
// - CREATE_OBJECT / CLOSE_OBJECT (current input/output + new output screen buffers)
// - RAW_FLUSH returns success
// - USER_DEFINED: a growing subset needed by classic console clients:
//   - Get/SetMode, GetCP/SetCP, GetNumberOfInputEvents
//   - WriteConsole / ReadConsole (byte passthrough + UTF-16 -> UTF-8 for output)
//   - Screen buffer state (Get/SetCursorInfo, SetCursorPosition, Get/SetScreenBufferInfo,
//     SetTextAttribute, SetScreenBufferSize, GetLargestWindowSize, SetWindowInfo)
//   - Output buffer contents (FillConsoleOutput, Read/WriteConsoleOutputString, Read/WriteConsoleOutput)
//   - ScrollConsoleScreenBuffer and Get/SetTitle
// - other operations are rejected with STATUS_NOT_IMPLEMENTED
//
// See also:
// - `new/docs/conhost_behavior_imitation_matrix.md`
// - `new/docs/design/condrv_raw_io_parity.md`
// - `new/tests/condrv_server_dispatch_tests.cpp` (large unit-test suite)

#include "condrv/condrv_api_message.hpp"
#include "condrv/condrv_device_comm.hpp"
#include "condrv/command_history.hpp"
#include "condrv/screen_buffer_snapshot.hpp"
#include "condrv/vt_input_decoder.hpp"
#include "core/assert.hpp"
#include "core/host_signals.hpp"
#include "core/handle_view.hpp"
#include "core/ntstatus.hpp"

#include <Windows.h>
#include <ntcon.h>

#include <atomic>
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace oc::logging
{
    class Logger;
}

 namespace oc::condrv
 {
    class ScreenBuffer;
    class ServerState;

    namespace detail
    {
        struct VtCsiSequence final
        {
            wchar_t final{};
            bool private_marker{};
            bool exclamation_marker{};
            std::array<unsigned, 16> params{};
            size_t param_count{};
        };

        struct VtOutputParseState final
        {
            enum class Phase : unsigned char
            {
                ground,
                escape,
                esc_dispatch,
                csi,
                osc,
                osc_escape,
                string,
                string_escape,
            };

            Phase phase{ Phase::ground };

            // ESC dispatch parsing state: intermediate bytes (0x20..0x2F) followed by a final byte (0x30..0x7E).
            std::array<wchar_t, 8> esc_intermediates{};
            size_t esc_intermediate_count{};
            size_t esc_length{};

            // CSI parsing state.
            VtCsiSequence csi{};
            unsigned csi_current{};
            bool csi_have_digits{};
            bool csi_last_was_separator{};
            size_t csi_length{};

            // OSC parsing state (only a small subset is dispatched; the rest is consumed).
            unsigned osc_param{};
            bool osc_param_have_digits{};
            bool osc_in_param{ true };
            unsigned osc_action{};
            bool osc_capture_payload{};
            std::array<wchar_t, 4096> osc_payload{};
            size_t osc_payload_length{};
        };
    }

    template<typename HostIo>
    inline void apply_text_to_screen_buffer(
        ScreenBuffer& screen_buffer,
        std::wstring_view text,
        ULONG output_mode,
        ServerState* title_state,
        HostIo* host_io) noexcept;

    class ScreenBuffer final
    {
    public:
        struct Settings final
        {
            COORD buffer_size{};
            COORD cursor_position{};
            COORD scroll_position{}; // Top-left corner of the viewport within the screen buffer.
            COORD window_size{};
            COORD maximum_window_size{};
            USHORT text_attributes{ 0x07 };
            ULONG cursor_size{ 25 };
            bool cursor_visible{ true };
            std::array<COLORREF, 16> color_table{};
        };

        [[nodiscard]] static Settings default_settings() noexcept;
        [[nodiscard]] static std::expected<std::shared_ptr<ScreenBuffer>, DeviceCommError> create(Settings settings) noexcept;
        [[nodiscard]] static std::expected<std::shared_ptr<ScreenBuffer>, DeviceCommError> create_blank_like(const ScreenBuffer& template_buffer) noexcept;

        explicit ScreenBuffer(Settings settings);

        [[nodiscard]] COORD screen_buffer_size() const noexcept;
        [[nodiscard]] bool set_screen_buffer_size(COORD size) noexcept;

        // Monotonically increasing revision counter used to detect visible changes.
        // Incremented on every mutation of buffer state or cells (best-effort).
        [[nodiscard]] uint64_t revision() const noexcept
        {
            return _revision;
        }

        [[nodiscard]] COORD cursor_position() const noexcept;
        void set_cursor_position(COORD position) noexcept;

        [[nodiscard]] SMALL_RECT window_rect() const noexcept;
        [[nodiscard]] COORD scroll_position() const noexcept;
        [[nodiscard]] bool set_window_rect(SMALL_RECT rect) noexcept;

        [[nodiscard]] COORD window_size() const noexcept;
        [[nodiscard]] bool set_window_size(COORD size) noexcept;
        void snap_window_to_cursor() noexcept;

        [[nodiscard]] COORD maximum_window_size() const noexcept;

        [[nodiscard]] USHORT text_attributes() const noexcept;
        [[nodiscard]] USHORT default_text_attributes() const noexcept;
        void set_text_attributes(USHORT attributes) noexcept;
        void set_default_text_attributes(USHORT attributes) noexcept;

        [[nodiscard]] ULONG cursor_size() const noexcept;
        [[nodiscard]] bool cursor_visible() const noexcept;
        void set_cursor_info(ULONG size, bool visible) noexcept;

        void save_cursor_state(COORD position, USHORT attributes, bool delayed_eol_wrap, bool origin_mode_enabled) noexcept;
        [[nodiscard]] bool restore_cursor_state(COORD& position, USHORT& attributes, bool& delayed_eol_wrap, bool& origin_mode_enabled) const noexcept;

        // Query and update VT autowrap state (DECAWM, CSI ? 7 h/l).
        [[nodiscard]] bool vt_autowrap_enabled() const noexcept
        {
            return _vt_autowrap_enabled;
        }

        void set_vt_autowrap_enabled(bool enabled) noexcept;

        // Query/update the delayed wrap state ("last column flag").
        //
        // When autowrap is enabled and a printable character is written in the final column, terminals
        // typically clamp the cursor to the final column and set a "delayed wrap" flag. The actual wrap
        // (line feed + carriage return) is performed only when another printable character is output, and
        // only if the cursor did not move away from the recorded position in the meantime.
        [[nodiscard]] std::optional<COORD> vt_delayed_wrap_position() const noexcept
        {
            return _vt_delayed_wrap_position;
        }

        void set_vt_delayed_wrap_position(std::optional<COORD> position) noexcept;

        // Query and update VT origin mode (DECOM, CSI ? 6 h/l).
        [[nodiscard]] bool vt_origin_mode_enabled() const noexcept
        {
            return _vt_origin_mode_enabled;
        }

        void set_vt_origin_mode_enabled(bool enabled) noexcept;

        // Query and update VT insert/replace mode (IRM, CSI 4 h/l).
        //
        // When enabled, printable output inserts cells at the cursor by shifting the
        // current line to the right. When disabled, printable output overwrites cells.
        [[nodiscard]] bool vt_insert_mode_enabled() const noexcept
        {
            return _vt_insert_mode_enabled;
        }

        void set_vt_insert_mode_enabled(bool enabled) noexcept;

        [[nodiscard]] const std::array<COLORREF, 16>& color_table() const noexcept;
        void set_color_table(const COLORREF (&table)[16]) noexcept;

        // The VT output parser supports a small subset of terminal state. One of the most
        // visible pieces is the DECSTBM "scroll region" (top/bottom margins). When set,
        // line feeds at the bottom margin scroll only within the region instead of the
        // full buffer, enabling classic full-screen TUIs to keep status lines fixed.
        //
        // The margins are stored in buffer coordinates and are inclusive. When `nullopt`,
        // the active scroll region is the full buffer height.
        struct VtVerticalMargins final
        {
            SHORT top{};    // inclusive, 0-based
            SHORT bottom{}; // inclusive, 0-based
        };

        [[nodiscard]] std::optional<VtVerticalMargins> vt_vertical_margins() const noexcept;
        void set_vt_vertical_margins(std::optional<VtVerticalMargins> margins) noexcept;

        // Query whether the buffer currently represents the VT alternate screen buffer (DECSET 1049).
        // When active, `_vt_main_backup` holds the preserved main-screen state.
        [[nodiscard]] bool vt_using_alternate_screen_buffer() const noexcept
        {
            return _vt_main_backup.has_value();
        }

        // Enable or disable the VT alternate screen buffer.
        // Returns false only when enabling fails due to allocation failure; the buffer remains unchanged.
        [[nodiscard]] bool set_vt_using_alternate_screen_buffer(
            bool enable,
            wchar_t fill_character,
            USHORT fill_attributes) noexcept;

        [[nodiscard]] bool write_cell(COORD coord, wchar_t character, USHORT attributes) noexcept;
        [[nodiscard]] bool insert_cell(COORD coord, wchar_t character, USHORT attributes) noexcept;

        [[nodiscard]] size_t fill_output_characters(COORD origin, wchar_t value, size_t length) noexcept;
        [[nodiscard]] size_t fill_output_attributes(COORD origin, USHORT value, size_t length) noexcept;

        [[nodiscard]] size_t write_output_characters(COORD origin, std::span<const wchar_t> text) noexcept;
        [[nodiscard]] size_t write_output_attributes(COORD origin, std::span<const USHORT> attributes) noexcept;
        [[nodiscard]] size_t write_output_ascii(COORD origin, std::span<const std::byte> bytes) noexcept;

        [[nodiscard]] size_t read_output_characters(COORD origin, std::span<wchar_t> dest) const noexcept;
        [[nodiscard]] size_t read_output_attributes(COORD origin, std::span<USHORT> dest) const noexcept;
        [[nodiscard]] size_t read_output_ascii(COORD origin, std::span<std::byte> dest) const noexcept;

        [[nodiscard]] size_t write_output_char_info_rect(SMALL_RECT region, std::span<const CHAR_INFO> records, bool unicode) noexcept;
        [[nodiscard]] size_t read_output_char_info_rect(SMALL_RECT region, std::span<CHAR_INFO> records, bool unicode) const noexcept;

        [[nodiscard]] bool scroll_screen_buffer(
            SMALL_RECT scroll_rectangle,
            SMALL_RECT clip_rectangle,
            COORD destination_origin,
            wchar_t fill_character,
            USHORT fill_attributes) noexcept;

    private:
        template<typename HostIo>
        friend void apply_text_to_screen_buffer(
            ScreenBuffer& screen_buffer,
            std::wstring_view text,
            ULONG output_mode,
            ServerState* title_state,
            HostIo* host_io) noexcept;

        struct ScreenCell final
        {
            wchar_t character{ L' ' };
            USHORT attributes{ 0x07 };
        };

        struct SavedCursorState final
        {
            COORD position{};
            USHORT attributes{};
            bool delayed_eol_wrap{ false };
            bool origin_mode_enabled{ false };
        };

        // When the VT alternate screen buffer is active, we preserve the main buffer state here.
        struct VtAlternateBufferBackup final
        {
            std::vector<ScreenCell> cells;
            COORD cursor_position{};
            USHORT text_attributes{};
            USHORT default_text_attributes{};
            ULONG cursor_size{};
            bool cursor_visible{};
            std::optional<SavedCursorState> saved_cursor_state{};
            std::optional<VtVerticalMargins> vt_vertical_margins{};
            std::optional<COORD> vt_delayed_wrap_position{};
            bool vt_origin_mode_enabled{ false };
        };

        [[nodiscard]] bool coord_in_range(COORD coord) const noexcept;
        [[nodiscard]] size_t linear_index(COORD coord) const noexcept;

        void touch() noexcept
        {
            ++_revision;
        }

        COORD _buffer_size{};
        COORD _cursor_position{};
        SMALL_RECT _window_rect{};
        COORD _maximum_window_size{};
        USHORT _text_attributes{ 0x07 };
        USHORT _default_text_attributes{ 0x07 };
        ULONG _cursor_size{ 25 };
        bool _cursor_visible{ true };
        std::array<COLORREF, 16> _color_table{};
        std::optional<SavedCursorState> _saved_cursor_state{};
        std::optional<VtVerticalMargins> _vt_vertical_margins{};
        std::optional<VtAlternateBufferBackup> _vt_main_backup{};
        bool _vt_autowrap_enabled{ true };
        std::optional<COORD> _vt_delayed_wrap_position{};
        bool _vt_origin_mode_enabled{ false };
        bool _vt_insert_mode_enabled{ false };
        detail::VtOutputParseState _vt_output_parse_state{};
        std::vector<ScreenCell> _cells;
        uint64_t _revision{ 0 };
    };

    struct NullHostIo final
    {
        [[nodiscard]] std::expected<size_t, DeviceCommError> write_output_bytes(std::span<const std::byte> bytes) noexcept
        {
            return bytes.size();
        }

        [[nodiscard]] std::expected<size_t, DeviceCommError> read_input_bytes(std::span<std::byte> /*dest*/) noexcept
        {
            return size_t{ 0 };
        }

        [[nodiscard]] std::expected<size_t, DeviceCommError> peek_input_bytes(std::span<std::byte> /*dest*/) noexcept
        {
            return size_t{ 0 };
        }

        [[nodiscard]] size_t input_bytes_available() const noexcept
        {
            return 0;
        }

        [[nodiscard]] bool input_disconnected() const noexcept
        {
            return true;
        }

        [[nodiscard]] bool inject_input_bytes(std::span<const std::byte> /*bytes*/) noexcept
        {
            return true;
        }

        [[nodiscard]] bool vt_should_answer_queries() const noexcept
        {
            return true;
        }

        [[nodiscard]] std::expected<void, DeviceCommError> flush_input_buffer() noexcept
        {
            return {};
        }

        [[nodiscard]] std::expected<bool, DeviceCommError> wait_for_input(const DWORD /*timeout_ms*/) noexcept
        {
            return false;
        }

        [[nodiscard]] std::expected<void, DeviceCommError> send_end_task(
            const DWORD /*process_id*/,
            const DWORD /*event_type*/,
            const DWORD /*ctrl_flags*/) noexcept
        {
            return {};
        }
    };

    struct ServerError final
    {
        std::wstring context;
        DWORD win32_error{ ERROR_GEN_FAILURE };
    };

    struct DispatchOutcome final
    {
        bool request_exit{ false };
        // When true, the caller must not complete the IO yet. The message must be retried later.
        bool reply_pending{ false };
    };

    enum class ObjectKind : unsigned char
    {
        input,
        output,
    };

    class PendingInputBytes final
    {
    public:
        PendingInputBytes() noexcept = default;

        [[nodiscard]] bool empty() const noexcept
        {
            return _size == 0;
        }

        [[nodiscard]] size_t size() const noexcept
        {
            return _size;
        }

        [[nodiscard]] size_t capacity() const noexcept
        {
            return _storage.size();
        }

        [[nodiscard]] std::span<const std::byte> bytes() const noexcept
        {
            return std::span<const std::byte>(_storage.data(), _size);
        }

        void clear() noexcept
        {
            _size = 0;
        }

        [[nodiscard]] bool append(const std::span<const std::byte> data) noexcept
        {
            if (data.empty())
            {
                return true;
            }

            if (data.size() > capacity() - _size)
            {
                return false;
            }

            std::memcpy(_storage.data() + _size, data.data(), data.size());
            _size += data.size();
            return true;
        }

        void consume_prefix(const size_t count) noexcept
        {
            const size_t to_consume = std::min(count, _size);
            if (to_consume == 0)
            {
                return;
            }

            _size -= to_consume;
            if (_size != 0)
            {
                std::memmove(_storage.data(), _storage.data() + to_consume, _size);
            }
        }

    private:
        std::array<std::byte, 64> _storage{};
        size_t _size{ 0 };
    };

    struct ObjectHandle final
    {
        ObjectKind kind{ ObjectKind::input };
        ACCESS_MASK desired_access{};
        ULONG share_mode{};
        ULONG_PTR owning_process{}; // ConDrv "process handle" cookie (opaque to the driver).
        std::shared_ptr<ScreenBuffer> screen_buffer{};

        // When UTF-8/code-page decoding produces a surrogate pair but a caller-provided buffer can hold
        // only one UTF-16 code unit, we consume the corresponding bytes and return the first unit while
        // keeping the second unit here for a subsequent read. This matches the inbox host's "one input
        // record per UTF-16 unit" behavior without requiring a full `INPUT_RECORD` queue yet.
        std::optional<wchar_t> decoded_input_pending{};

        // When the head of the input byte stream contains an incomplete UTF-8/DBCS sequence, draining it
        // into this prefix buffer avoids repeatedly treating "some bytes exist" as "a full character exists".
        // This buffer persists across reply-pending waits so reads can resume when more bytes arrive.
        PendingInputBytes pending_input_bytes{};

        // Pending cooked-read output for line-input `ReadConsole` calls.
        // Stored as UTF-16 code units so reads can be satisfied incrementally when
        // the caller's output buffer is smaller than the completed line.
        std::wstring cooked_read_pending{};

        // Cooked line-input state that persists across reply-pending waits. We append decoded characters here
        // until we observe CR/LF termination, at which point we move the completed line into `cooked_read_pending`.
        std::wstring cooked_line_in_progress{};

        // Cooked line-input editing cursor within `cooked_line_in_progress`.
        // Stored as a UTF-16 code-unit index, but maintained so it never points inside a surrogate pair.
        size_t cooked_line_cursor{};

        // Cooked line-input insert mode. When false, typed characters overwrite existing units at the cursor.
        bool cooked_insert_mode{ true };
    };

    struct ProcessState final
    {
        DWORD pid{};
        DWORD tid{};
        unsigned long long connect_sequence{};

        ULONG_PTR process_handle{};
        ULONG_PTR input_handle{};
        ULONG_PTR output_handle{};
    };

    class ServerState final
    {
    public:
        struct TransparentWStringHash final
        {
            using is_transparent = void;

            [[nodiscard]] size_t operator()(const std::wstring& value) const noexcept
            {
                return std::hash<std::wstring_view>{}(value);
            }

            [[nodiscard]] size_t operator()(const std::wstring_view value) const noexcept
            {
                return std::hash<std::wstring_view>{}(value);
            }
        };

        struct TransparentWStringEqual final
        {
            using is_transparent = void;

            [[nodiscard]] bool operator()(const std::wstring& left, const std::wstring& right) const noexcept
            {
                return left == right;
            }

            [[nodiscard]] bool operator()(const std::wstring_view left, const std::wstring_view right) const noexcept
            {
                return left == right;
            }

            [[nodiscard]] bool operator()(const std::wstring& left, const std::wstring_view right) const noexcept
            {
                return std::wstring_view(left) == right;
            }

            [[nodiscard]] bool operator()(const std::wstring_view left, const std::wstring& right) const noexcept
            {
                return left == std::wstring_view(right);
            }
        };

        ServerState() noexcept;
        ~ServerState() = default;

        ServerState(const ServerState&) = delete;
        ServerState& operator=(const ServerState&) = delete;

        [[nodiscard]] size_t process_count() const noexcept;

        [[nodiscard]] std::expected<ConnectionInformation, DeviceCommError> connect_client(
            DWORD pid,
            DWORD tid,
            std::wstring_view app_name = {}) noexcept;
        [[nodiscard]] bool disconnect_client(ULONG_PTR process_handle) noexcept;

        [[nodiscard]] std::expected<ULONG_PTR, DeviceCommError> create_object(ObjectHandle object) noexcept;
        [[nodiscard]] bool close_object(ULONG_PTR handle_id) noexcept;

        [[nodiscard]] bool has_process(ULONG_PTR process_handle) const noexcept;
        [[nodiscard]] ObjectHandle* find_object(ULONG_PTR handle_id) noexcept;

        template<typename Fn>
        void for_each_process(Fn&& fn) const noexcept
        {
            for (const auto& entry : _processes)
            {
                const auto& process = entry.second;
                if (process != nullptr)
                {
                    fn(*process);
                }
            }
        }

        [[nodiscard]] ULONG input_mode() const noexcept;
        [[nodiscard]] ULONG output_mode() const noexcept;
        void set_input_mode(ULONG mode) noexcept;
        void set_output_mode(ULONG mode) noexcept;

        [[nodiscard]] ULONG input_code_page() const noexcept;
        [[nodiscard]] ULONG output_code_page() const noexcept;
        void set_input_code_page(ULONG code_page) noexcept;
        void set_output_code_page(ULONG code_page) noexcept;

        [[nodiscard]] std::shared_ptr<ScreenBuffer> active_screen_buffer() const noexcept;
        [[nodiscard]] bool set_active_screen_buffer(std::shared_ptr<ScreenBuffer> buffer) noexcept;
        [[nodiscard]] std::expected<std::shared_ptr<ScreenBuffer>, DeviceCommError> create_screen_buffer_like_active() noexcept;

        [[nodiscard]] std::wstring_view title(bool original) const noexcept;
        [[nodiscard]] bool set_title(std::wstring title) noexcept;
        [[nodiscard]] bool set_title(std::wstring_view title) noexcept;

        [[nodiscard]] ULONG history_buffer_size() const noexcept;
        [[nodiscard]] ULONG history_buffer_count() const noexcept;
        [[nodiscard]] ULONG history_flags() const noexcept;
        void set_history_info(ULONG buffer_size, ULONG buffer_count, ULONG flags) noexcept;

        [[nodiscard]] CommandHistory* try_command_history_for_process(ULONG_PTR process_handle) noexcept;
        [[nodiscard]] CommandHistory* try_command_history_for_exe(std::wstring_view exe_name) noexcept;
        [[nodiscard]] const CommandHistory* try_command_history_for_exe(std::wstring_view exe_name) const noexcept;

        void add_command_history_for_process(ULONG_PTR process_handle, std::wstring_view command, bool suppress_duplicates) noexcept;
        void expunge_command_history(std::wstring_view exe_name) noexcept;
        void set_command_history_number_of_commands(std::wstring_view exe_name, size_t max_commands) noexcept;

        [[nodiscard]] ULONG font_index() const noexcept;
        [[nodiscard]] COORD font_size() const noexcept;
        void fill_current_font(CONSOLE_CURRENTFONT_MSG& body) const noexcept;
        void apply_current_font(const CONSOLE_CURRENTFONT_MSG& body) noexcept;

        void set_cursor_mode(bool blink, bool db_enable) noexcept;
        [[nodiscard]] bool cursor_blink() const noexcept;
        [[nodiscard]] bool cursor_db_enable() const noexcept;

        void set_nls_mode(ULONG mode) noexcept;
        [[nodiscard]] ULONG nls_mode() const noexcept;

        void set_menu_close(bool enable) noexcept;
        [[nodiscard]] bool menu_close() const noexcept;

        void set_key_shortcuts(bool enabled, unsigned char reserved_keys) noexcept;

        void set_os2_registered(bool registered) noexcept;
        [[nodiscard]] bool os2_registered() const noexcept;

        void set_os2_oem_format(bool enabled) noexcept;
        [[nodiscard]] bool os2_oem_format() const noexcept;

        [[nodiscard]] std::expected<void, DeviceCommError> set_alias(
            std::wstring exe_name,
            std::wstring source,
            std::wstring target) noexcept;

        [[nodiscard]] std::optional<std::wstring_view> try_get_alias(
            std::wstring_view exe_name,
            std::wstring_view source) const noexcept;

        template<typename Fn>
        void for_each_alias(std::wstring_view exe_name, Fn&& fn) const noexcept
        {
            const auto iter = _aliases.find(exe_name);
            if (iter == _aliases.end())
            {
                return;
            }

            const auto& table = iter->second;
            for (const auto& alias_entry : table)
            {
                fn(std::wstring_view(alias_entry.first), std::wstring_view(alias_entry.second));
            }
        }

        template<typename Fn>
        void for_each_alias_exe(Fn&& fn) const noexcept
        {
            for (const auto& entry : _aliases)
            {
                fn(std::wstring_view(entry.first));
            }
        }

    private:
        [[nodiscard]] static std::expected<std::unique_ptr<ProcessState>, DeviceCommError> make_process_state(DWORD pid, DWORD tid) noexcept;
        [[nodiscard]] static std::expected<std::unique_ptr<ObjectHandle>, DeviceCommError> make_object_handle(ObjectHandle object) noexcept;

        using AliasTable = std::unordered_map<std::wstring, std::wstring, TransparentWStringHash, TransparentWStringEqual>;

        std::unordered_map<ULONG_PTR, std::unique_ptr<ProcessState>> _processes;
        std::unordered_map<ULONG_PTR, std::unique_ptr<ObjectHandle>> _objects;
        std::unordered_map<std::wstring, AliasTable, TransparentWStringHash, TransparentWStringEqual> _aliases;

        ULONG _input_mode{ ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS };
        ULONG _output_mode{ ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT };
        ULONG _input_code_page{ 0 };
        ULONG _output_code_page{ 0 };

        std::wstring _title;
        std::wstring _original_title;

        ULONG _history_buffer_size{ 50 };
        ULONG _history_buffer_count{ 4 };
        ULONG _history_flags{ 0 };
        CommandHistoryPool _command_histories{};

        ULONG _font_index{ 0 };
        COORD _font_size{};
        ULONG _font_family{ FF_MODERN };
        ULONG _font_weight{ FW_NORMAL };
        std::array<wchar_t, LF_FACESIZE> _font_face_name{};

        bool _cursor_blink{ true };
        bool _cursor_db_enable{ false };
        ULONG _nls_mode{ 0 };
        bool _menu_close{ true };
        bool _key_shortcuts_enabled{ false };
        unsigned char _reserved_keys{ 0 };
        bool _os2_registered{ false };
        bool _os2_oem_format{ false };

        std::shared_ptr<ScreenBuffer> _main_screen_buffer;
        std::shared_ptr<ScreenBuffer> _active_screen_buffer;
        unsigned long long _next_connect_sequence{ 1 };
    };

    [[nodiscard]] inline std::expected<std::wstring, DeviceCommError> decode_console_string(
        const bool unicode,
        const std::span<const std::byte> bytes,
        const UINT code_page,
        const wchar_t* const context) noexcept
    {
        if (context == nullptr)
        {
            return std::unexpected(DeviceCommError{ .context = L"decode_console_string context was null", .win32_error = ERROR_INVALID_PARAMETER });
        }

        if (unicode)
        {
            if ((bytes.size() % sizeof(wchar_t)) != 0)
            {
                return std::unexpected(DeviceCommError{ .context = context, .win32_error = ERROR_INVALID_DATA });
            }

            const size_t length = bytes.size() / sizeof(wchar_t);
            std::wstring out;
            try
            {
                out.resize(length);
            }
            catch (...)
            {
                return std::unexpected(DeviceCommError{ .context = context, .win32_error = ERROR_OUTOFMEMORY });
            }

            if (length != 0)
            {
                std::memcpy(out.data(), bytes.data(), bytes.size());
            }

            return out;
        }

        if (bytes.empty())
        {
            return std::wstring{};
        }

        const auto input_size = bytes.size();
        if (input_size > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ERROR_INVALID_DATA });
        }

        const int required = ::MultiByteToWideChar(
            code_page,
            0,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(input_size),
            nullptr,
            0);
        if (required <= 0)
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ::GetLastError() });
        }

        std::wstring out;
        try
        {
            out.resize(static_cast<size_t>(required));
        }
        catch (...)
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ERROR_OUTOFMEMORY });
        }

        const int converted = ::MultiByteToWideChar(
            code_page,
            0,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(input_size),
            out.data(),
            required);
        if (converted != required)
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ::GetLastError() });
        }

        return out;
    }

    [[nodiscard]] inline std::expected<std::wstring, DeviceCommError> fold_to_lower_invariant(
        const std::wstring_view value,
        const wchar_t* const context) noexcept
    {
        if (context == nullptr)
        {
            return std::unexpected(DeviceCommError{ .context = L"fold_to_lower_invariant context was null", .win32_error = ERROR_INVALID_PARAMETER });
        }

        if (value.empty())
        {
            return std::wstring{};
        }

        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ERROR_INVALID_DATA });
        }

        const int required = ::LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr,
            0);
        if (required <= 0)
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ::GetLastError() });
        }

        std::wstring out;
        try
        {
            out.resize(static_cast<size_t>(required));
        }
        catch (...)
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ERROR_OUTOFMEMORY });
        }

        const int converted = ::LCMapStringEx(
            LOCALE_NAME_INVARIANT,
            LCMAP_LOWERCASE,
            value.data(),
            static_cast<int>(value.size()),
            out.data(),
            required,
            nullptr,
            nullptr,
            0);
        if (converted <= 0)
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ::GetLastError() });
        }

        if (!out.empty() && out.back() == L'\0')
        {
            out.pop_back();
        }

        return out;
    }

    struct InputDecodeChunk final
    {
        std::array<wchar_t, 2> chars{};
        size_t char_count{};
        size_t bytes_consumed{};
    };

    enum class InputDecodeOutcome : unsigned char
    {
        produced,
        need_more_data,
    };

    [[nodiscard]] inline bool key_event_matches_ctrl_c(const KEY_EVENT_RECORD& key) noexcept
    {
        constexpr DWORD ctrl_mask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        constexpr DWORD alt_mask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
        return (key.dwControlKeyState & ctrl_mask) != 0 &&
            (key.dwControlKeyState & alt_mask) == 0 &&
            key.wVirtualKeyCode == 'C';
    }

    [[nodiscard]] inline bool key_event_matches_ctrl_break(const KEY_EVENT_RECORD& key) noexcept
    {
        constexpr DWORD ctrl_mask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        constexpr DWORD alt_mask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
        return (key.dwControlKeyState & ctrl_mask) != 0 &&
            (key.dwControlKeyState & alt_mask) == 0 &&
            key.wVirtualKeyCode == VK_CANCEL;
    }

    [[nodiscard]] inline KEY_EVENT_RECORD make_simple_character_key_event(const wchar_t value) noexcept
    {
        KEY_EVENT_RECORD key{};
        key.bKeyDown = TRUE;
        key.wRepeatCount = 1;
        key.wVirtualKeyCode = 0;
        key.wVirtualScanCode = 0;
        key.dwControlKeyState = 0;
        key.uChar.UnicodeChar = value;
        return key;
    }

    [[nodiscard]] inline INPUT_RECORD make_input_record_from_key(const KEY_EVENT_RECORD& key, const bool unicode) noexcept
    {
        INPUT_RECORD record{};
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent = key;
        if (!unicode)
        {
            const wchar_t value = key.uChar.UnicodeChar;
            record.Event.KeyEvent.uChar.AsciiChar = value <= 0xFF ? static_cast<char>(value) : static_cast<char>('?');
        }
        return record;
    }

    [[nodiscard]] inline InputDecodeOutcome decode_one_console_input_unit(
        UINT code_page,
        std::span<const std::byte> bytes,
        InputDecodeChunk& out) noexcept;

    [[nodiscard]] inline InputDecodeOutcome decode_one_input_token(
        const UINT code_page,
        const std::span<const std::byte> bytes,
        vt_input::DecodedToken& out) noexcept
    {
        out = {};

        const auto vt_outcome = vt_input::try_decode_vt(bytes, out);
        if (vt_outcome == vt_input::DecodeResult::produced)
        {
            return InputDecodeOutcome::produced;
        }
        if (vt_outcome == vt_input::DecodeResult::need_more_data)
        {
            return InputDecodeOutcome::need_more_data;
        }

        InputDecodeChunk chunk{};
        const auto outcome = decode_one_console_input_unit(code_page, bytes, chunk);
        if (outcome == InputDecodeOutcome::need_more_data)
        {
            return outcome;
        }

        out.kind = vt_input::TokenKind::text_units;
        out.bytes_consumed = chunk.bytes_consumed;
        out.text.chars = chunk.chars;
        out.text.char_count = chunk.char_count;
        out.text.bytes_consumed = chunk.bytes_consumed;
        return InputDecodeOutcome::produced;
    }

    [[nodiscard]] inline InputDecodeOutcome decode_one_console_input_unit(
        const UINT code_page,
        const std::span<const std::byte> bytes,
        InputDecodeChunk& out) noexcept
    {
        out = {};
        if (bytes.empty())
        {
            return InputDecodeOutcome::need_more_data;
        }

        const auto replacement = static_cast<wchar_t>(0xFFFD);
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());

        if (code_page == CP_UTF8)
        {
            const unsigned char b0 = data[0];
            size_t sequence = 0;
            if (b0 < 0x80)
            {
                sequence = 1;
            }
            else if ((b0 & 0xE0) == 0xC0)
            {
                sequence = 2;
            }
            else if ((b0 & 0xF0) == 0xE0)
            {
                sequence = 3;
            }
            else if ((b0 & 0xF8) == 0xF0)
            {
                sequence = 4;
            }
            else
            {
                out.chars[0] = replacement;
                out.char_count = 1;
                out.bytes_consumed = 1;
                return InputDecodeOutcome::produced;
            }

            if (bytes.size() < sequence)
            {
                return InputDecodeOutcome::need_more_data;
            }

            wchar_t decoded[2]{};
            const int converted = ::MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(sequence),
                decoded,
                static_cast<int>(std::size(decoded)));
            if (converted <= 0)
            {
                out.chars[0] = replacement;
                out.char_count = 1;
                out.bytes_consumed = 1;
                return InputDecodeOutcome::produced;
            }

            out.bytes_consumed = sequence;
            out.char_count = static_cast<size_t>(converted);
            for (int i = 0; i < converted; ++i)
            {
                out.chars[static_cast<size_t>(i)] = decoded[i];
            }
            return InputDecodeOutcome::produced;
        }

        const unsigned char b0 = data[0];
        size_t sequence = ::IsDBCSLeadByteEx(code_page, static_cast<char>(b0)) ? 2 : 1;
        if (bytes.size() < sequence)
        {
            return InputDecodeOutcome::need_more_data;
        }

        wchar_t decoded[2]{};
        const int converted = ::MultiByteToWideChar(
            code_page,
            0,
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(sequence),
            decoded,
            static_cast<int>(std::size(decoded)));
        if (converted <= 0)
        {
            out.chars[0] = replacement;
            out.char_count = 1;
            out.bytes_consumed = 1;
            return InputDecodeOutcome::produced;
        }

        out.bytes_consumed = sequence;
        out.char_count = static_cast<size_t>(converted);
        for (int i = 0; i < converted; ++i)
        {
            out.chars[static_cast<size_t>(i)] = decoded[i];
        }
        return InputDecodeOutcome::produced;
    }

    struct InputDecodeSpanResult final
    {
        size_t bytes_consumed{};
        size_t units_written{};
    };

    [[nodiscard]] inline InputDecodeSpanResult decode_console_input_bytes_to_wchars(
        const UINT code_page,
        const std::span<const std::byte> bytes,
        const std::span<wchar_t> dest,
        const bool processed_input) noexcept
    {
        InputDecodeSpanResult result{};

        size_t offset = 0;
        size_t written = 0;
        while (written < dest.size() && offset < bytes.size())
        {
            InputDecodeChunk chunk{};
            const auto outcome = decode_one_console_input_unit(code_page, bytes.subspan(offset), chunk);
            if (outcome == InputDecodeOutcome::need_more_data)
            {
                break;
            }

            if (chunk.char_count == 0 || chunk.bytes_consumed == 0)
            {
                break;
            }

            if (processed_input && chunk.char_count == 1 && chunk.chars[0] == static_cast<wchar_t>(0x0003))
            {
                // Ctrl+C is a processed control event: consume it but do not return it as input.
                offset += chunk.bytes_consumed;
                continue;
            }

            if (chunk.char_count > dest.size() - written)
            {
                break;
            }

            for (size_t i = 0; i < chunk.char_count; ++i)
            {
                dest[written + i] = chunk.chars[i];
            }

            written += chunk.char_count;
            offset += chunk.bytes_consumed;
        }

        result.bytes_consumed = offset;
        result.units_written = written;
        return result;
    }

    [[nodiscard]] inline InputDecodeSpanResult decode_console_input_bytes_to_key_events(
        const UINT code_page,
        const std::span<const std::byte> bytes,
        const std::span<INPUT_RECORD> dest,
        const bool unicode) noexcept
    {
        InputDecodeSpanResult result{};

        size_t offset = 0;
        size_t written = 0;
        while (written < dest.size() && offset < bytes.size())
        {
            InputDecodeChunk chunk{};
            const auto outcome = decode_one_console_input_unit(code_page, bytes.subspan(offset), chunk);
            if (outcome == InputDecodeOutcome::need_more_data)
            {
                break;
            }

            if (chunk.char_count == 0 || chunk.bytes_consumed == 0)
            {
                break;
            }

            if (chunk.char_count > dest.size() - written)
            {
                break;
            }

            for (size_t i = 0; i < chunk.char_count; ++i)
            {
                INPUT_RECORD record{};
                record.EventType = KEY_EVENT;
                record.Event.KeyEvent.bKeyDown = TRUE;
                record.Event.KeyEvent.wRepeatCount = 1;
                record.Event.KeyEvent.wVirtualKeyCode = 0;
                record.Event.KeyEvent.wVirtualScanCode = 0;
                record.Event.KeyEvent.dwControlKeyState = 0;

                const wchar_t value = chunk.chars[i];
                if (unicode)
                {
                    record.Event.KeyEvent.uChar.UnicodeChar = value;
                }
                else
                {
                    record.Event.KeyEvent.uChar.AsciiChar = value <= 0xFF ? static_cast<char>(value) : static_cast<char>('?');
                }

                dest[written + i] = record;
            }

            written += chunk.char_count;
            offset += chunk.bytes_consumed;
        }

        result.bytes_consumed = offset;
        result.units_written = written;
        return result;
    }

    [[nodiscard]] inline size_t count_console_input_units_utf8(const std::span<const std::byte> bytes) noexcept
    {
        size_t offset = 0;
        size_t count = 0;
        while (offset < bytes.size())
        {
            InputDecodeChunk chunk{};
            const auto outcome = decode_one_console_input_unit(CP_UTF8, bytes.subspan(offset), chunk);
            if (outcome == InputDecodeOutcome::need_more_data)
            {
                break;
            }

            if (chunk.bytes_consumed == 0)
            {
                break;
            }

            count += chunk.char_count == 0 ? 1 : chunk.char_count;
            offset += chunk.bytes_consumed;
        }
        return count;
    }

    template<typename HostIo>
    inline void apply_text_to_screen_buffer(
        ScreenBuffer& screen_buffer,
        const std::wstring_view text,
        const ULONG output_mode,
        ServerState* const title_state,
        HostIo* const host_io) noexcept
    {
        COORD cursor = screen_buffer.cursor_position();
        const COORD buffer_size = screen_buffer.screen_buffer_size();
        if (buffer_size.X <= 0 || buffer_size.Y <= 0)
        {
            return;
        }

        USHORT attributes = screen_buffer.text_attributes();
        const USHORT default_attributes = screen_buffer.default_text_attributes();
        const bool processed_output = (output_mode & ENABLE_PROCESSED_OUTPUT) != 0;
        const bool wrap_at_eol_output_mode = (output_mode & ENABLE_WRAP_AT_EOL_OUTPUT) != 0;
        const bool vt_processing = (output_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        const bool disable_newline_auto_return = (output_mode & DISABLE_NEWLINE_AUTO_RETURN) != 0;

        auto vt_vertical_margins = screen_buffer.vt_vertical_margins();
        const bool original_vt_autowrap = screen_buffer.vt_autowrap_enabled();
        bool vt_autowrap = original_vt_autowrap;
        auto vt_delayed_wrap_position = screen_buffer.vt_delayed_wrap_position();
        const bool original_vt_origin_mode = screen_buffer.vt_origin_mode_enabled();
        bool vt_origin_mode = original_vt_origin_mode;
        const bool original_vt_insert_mode = screen_buffer.vt_insert_mode_enabled();
        bool vt_insert_mode = original_vt_insert_mode;
        if (!vt_processing)
        {
            // Delayed wrap is only meaningful while VT processing is active.
            vt_delayed_wrap_position.reset();
            screen_buffer._vt_output_parse_state = {};
        }

        // Resolve the active VT scrolling region (DECSTBM) as an inclusive [top,bottom] range.
        // When margins are unset, the full buffer height is scrollable.
        const auto resolve_vertical_region = [&]() noexcept -> std::pair<SHORT, SHORT> {
            if (vt_vertical_margins)
            {
                const auto top = vt_vertical_margins->top;
                const auto bottom = vt_vertical_margins->bottom;
                if (top >= 0 && bottom >= top && bottom < buffer_size.Y)
                {
                    return { top, bottom };
                }
            }

            return { 0, static_cast<SHORT>(buffer_size.Y - 1) };
        };

        const auto scroll_region_up = [&](const SHORT top, const SHORT bottom, unsigned count) noexcept {
            if (buffer_size.X <= 0 || buffer_size.Y <= 1)
            {
                return;
            }

            if (top < 0 || bottom < top || bottom >= buffer_size.Y)
            {
                return;
            }

            if (count == 0)
            {
                count = 1;
            }

            const unsigned region_height = static_cast<unsigned>(static_cast<long>(bottom) - static_cast<long>(top) + 1);
            if (region_height <= 1)
            {
                return;
            }

            if (count >= region_height)
            {
                const size_t width = static_cast<size_t>(buffer_size.X);
                const size_t height = static_cast<size_t>(region_height);
                const size_t length = width * height;
                const COORD origin{ 0, top };
                (void)screen_buffer.fill_output_characters(origin, L' ', length);
                (void)screen_buffer.fill_output_attributes(origin, attributes, length);
                return;
            }

            const auto right = static_cast<SHORT>(buffer_size.X - 1);
            const auto scroll_top = static_cast<SHORT>(static_cast<long>(top) + static_cast<long>(count));

            SMALL_RECT scroll_rect{};
            scroll_rect.Left = 0;
            scroll_rect.Top = scroll_top;
            scroll_rect.Right = right;
            scroll_rect.Bottom = bottom;

            SMALL_RECT clip_rect{};
            clip_rect.Left = 0;
            clip_rect.Top = top;
            clip_rect.Right = right;
            clip_rect.Bottom = bottom;

            (void)screen_buffer.scroll_screen_buffer(
                scroll_rect,
                clip_rect,
                COORD{ 0, top },
                L' ',
                attributes);
        };

        const auto scroll_region_down = [&](const SHORT top, const SHORT bottom, unsigned count) noexcept {
            if (buffer_size.X <= 0 || buffer_size.Y <= 1)
            {
                return;
            }

            if (top < 0 || bottom < top || bottom >= buffer_size.Y)
            {
                return;
            }

            if (count == 0)
            {
                count = 1;
            }

            const unsigned region_height = static_cast<unsigned>(static_cast<long>(bottom) - static_cast<long>(top) + 1);
            if (region_height <= 1)
            {
                return;
            }

            if (count >= region_height)
            {
                const size_t width = static_cast<size_t>(buffer_size.X);
                const size_t height = static_cast<size_t>(region_height);
                const size_t length = width * height;
                const COORD origin{ 0, top };
                (void)screen_buffer.fill_output_characters(origin, L' ', length);
                (void)screen_buffer.fill_output_attributes(origin, attributes, length);
                return;
            }

            const auto right = static_cast<SHORT>(buffer_size.X - 1);
            const auto scroll_bottom = static_cast<SHORT>(static_cast<long>(bottom) - static_cast<long>(count));

            SMALL_RECT scroll_rect{};
            scroll_rect.Left = 0;
            scroll_rect.Top = top;
            scroll_rect.Right = right;
            scroll_rect.Bottom = scroll_bottom;

            SMALL_RECT clip_rect{};
            clip_rect.Left = 0;
            clip_rect.Top = top;
            clip_rect.Right = right;
            clip_rect.Bottom = bottom;

            const auto dest_top = static_cast<SHORT>(static_cast<long>(top) + static_cast<long>(count));
            (void)screen_buffer.scroll_screen_buffer(
                scroll_rect,
                clip_rect,
                COORD{ 0, dest_top },
                L' ',
                attributes);
        };

        auto line_feed = [&]() noexcept {
            const auto [top, bottom] = resolve_vertical_region();
            if (cursor.Y >= top && cursor.Y <= bottom)
            {
                if (cursor.Y == bottom)
                {
                    scroll_region_up(top, bottom, 1U);
                }
                else
                {
                    ++cursor.Y;
                }
                return;
            }

            ++cursor.Y;
            if (cursor.Y >= buffer_size.Y)
            {
                scroll_region_up(0, static_cast<SHORT>(buffer_size.Y - 1), 1U);
                cursor.Y = static_cast<SHORT>(buffer_size.Y - 1);
            }
        };

        auto reverse_line_feed = [&]() noexcept {
            const auto [top, bottom] = resolve_vertical_region();
            if (cursor.Y >= top && cursor.Y <= bottom)
            {
                if (cursor.Y == top)
                {
                    scroll_region_down(top, bottom, 1U);
                }
                else
                {
                    --cursor.Y;
                }
                return;
            }

            if (cursor.Y > 0)
            {
                --cursor.Y;
            }
            else
            {
                scroll_region_down(0, static_cast<SHORT>(buffer_size.Y - 1), 1U);
            }
        };

        auto advance_line = [&]() noexcept {
            cursor.X = 0;
            line_feed();
        };

        // Handle VT "delayed EOL wrap" (aka the "last column flag") when VT processing is enabled.
        //
        // Terminals that support autowrap clamp the cursor to the final column and set a delayed-wrap
        // flag when printing a glyph in that final column. The actual wrap (line feed + carriage return)
        // is performed only when the next printable glyph is output, and only if the cursor did not
        // move away from the recorded position in the meantime.
        const auto maybe_apply_delayed_wrap = [&]() noexcept {
            if (!vt_processing)
            {
                return;
            }

            if (!vt_delayed_wrap_position)
            {
                return;
            }

            if (vt_autowrap &&
                vt_delayed_wrap_position->X == cursor.X &&
                vt_delayed_wrap_position->Y == cursor.Y)
            {
                advance_line();
            }

            vt_delayed_wrap_position.reset();
        };

        const auto write_printable = [&](const wchar_t value) noexcept {
            maybe_apply_delayed_wrap();

            if (vt_processing && vt_insert_mode)
            {
                (void)screen_buffer.insert_cell(cursor, value, attributes);
            }
            else
            {
                (void)screen_buffer.write_cell(cursor, value, attributes);
            }

            if (vt_processing)
            {
                const SHORT last_column = static_cast<SHORT>(buffer_size.X - 1);
                if (cursor.X >= last_column)
                {
                    cursor.X = last_column;
                    if (vt_autowrap)
                    {
                        vt_delayed_wrap_position = cursor;
                    }
                }
                else
                {
                    ++cursor.X;
                }
                return;
            }

            ++cursor.X;
            if (cursor.X >= buffer_size.X)
            {
                if (wrap_at_eol_output_mode)
                {
                    advance_line();
                }
                else
                {
                    cursor.X = static_cast<SHORT>(buffer_size.X - 1);
                }
            }
        };

        const auto apply_sgr = [&](const auto& csi) noexcept {
            constexpr USHORT fg_color_mask = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
            constexpr USHORT bg_color_mask = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
            constexpr USHORT fg_full_mask = fg_color_mask | FOREGROUND_INTENSITY;
            constexpr USHORT bg_full_mask = bg_color_mask | BACKGROUND_INTENSITY;

            const auto set_foreground = [&](const unsigned color, const bool bright) noexcept {
                attributes = static_cast<USHORT>(attributes & ~(fg_color_mask | FOREGROUND_INTENSITY));
                if ((color & 0x01U) != 0)
                {
                    attributes |= FOREGROUND_RED;
                }
                if ((color & 0x02U) != 0)
                {
                    attributes |= FOREGROUND_GREEN;
                }
                if ((color & 0x04U) != 0)
                {
                    attributes |= FOREGROUND_BLUE;
                }
                if (bright)
                {
                    attributes |= FOREGROUND_INTENSITY;
                }
            };

            const auto set_background = [&](const unsigned color, const bool bright) noexcept {
                attributes = static_cast<USHORT>(attributes & ~(bg_color_mask | BACKGROUND_INTENSITY));
                if ((color & 0x01U) != 0)
                {
                    attributes |= BACKGROUND_RED;
                }
                if ((color & 0x02U) != 0)
                {
                    attributes |= BACKGROUND_GREEN;
                }
                if ((color & 0x04U) != 0)
                {
                    attributes |= BACKGROUND_BLUE;
                }
                if (bright)
                {
                    attributes |= BACKGROUND_INTENSITY;
                }
            };

            const auto set_palette_index = [&](const unsigned index, const bool foreground) noexcept {
                if (foreground)
                {
                    attributes = static_cast<USHORT>(attributes & ~fg_full_mask);
                    attributes = static_cast<USHORT>(attributes | static_cast<USHORT>(index & 0x0FU));
                }
                else
                {
                    attributes = static_cast<USHORT>(attributes & ~bg_full_mask);
                    attributes = static_cast<USHORT>(attributes | static_cast<USHORT>((index & 0x0FU) << 4));
                }
            };

            const auto clamp_byte = [](const unsigned value) noexcept -> unsigned {
                return value > 0xFFU ? 0xFFU : value;
            };

            const auto nearest_palette_index = [&](const unsigned red, const unsigned green, const unsigned blue) noexcept -> unsigned {
                const auto& table = screen_buffer.color_table();
                unsigned best_index = 0;
                std::uint32_t best_distance = std::numeric_limits<std::uint32_t>::max();
                for (unsigned i = 0; i < table.size(); ++i)
                {
                    const COLORREF color = table[i];
                    const int pr = static_cast<int>(color & 0xFF);
                    const int pg = static_cast<int>((color >> 8) & 0xFF);
                    const int pb = static_cast<int>((color >> 16) & 0xFF);

                    const int dr = pr - static_cast<int>(red);
                    const int dg = pg - static_cast<int>(green);
                    const int db = pb - static_cast<int>(blue);
                    const std::uint32_t distance =
                        static_cast<std::uint32_t>(dr * dr) +
                        static_cast<std::uint32_t>(dg * dg) +
                        static_cast<std::uint32_t>(db * db);
                    if (distance < best_distance)
                    {
                        best_distance = distance;
                        best_index = i;
                    }
                }
                return best_index;
            };

            const auto xterm_256_index_to_rgb = [&](unsigned index, unsigned& red, unsigned& green, unsigned& blue) noexcept {
                index = clamp_byte(index);
                if (index < 16)
                {
                    // 0-15 are the base palette. The caller handles these separately because they
                    // map to legacy SGR semantics (30-37/90-97) rather than raw Windows palette indices.
                    red = 0;
                    green = 0;
                    blue = 0;
                    return;
                }

                if (index >= 232)
                {
                    // Grayscale ramp: 8 + 10*(n-232)
                    const unsigned shade = 8U + 10U * (index - 232U);
                    red = shade;
                    green = shade;
                    blue = shade;
                    return;
                }

                // 6x6x6 color cube (16-231).
                const unsigned cube = index - 16U;
                const unsigned r = cube / 36U;
                const unsigned g = (cube / 6U) % 6U;
                const unsigned b = cube % 6U;
                const auto component = [](const unsigned value) noexcept -> unsigned {
                    return value == 0 ? 0U : 55U + 40U * value;
                };
                red = component(r);
                green = component(g);
                blue = component(b);
            };

            for (size_t i = 0; i < csi.param_count;)
            {
                const unsigned param = csi.params[i];
                if (param == 0)
                {
                    attributes = default_attributes;
                    ++i;
                    continue;
                }

                if (param == 1)
                {
                    // "Bold" is approximated by FOREGROUND_INTENSITY in the legacy attribute model.
                    attributes |= FOREGROUND_INTENSITY;
                    ++i;
                    continue;
                }

                if (param == 22)
                {
                    // Normal intensity (clears bold/faint).
                    attributes = static_cast<USHORT>(attributes & ~FOREGROUND_INTENSITY);
                    ++i;
                    continue;
                }

                if (param == 4)
                {
                    // Underline is represented by the legacy COMMON_LVB_UNDERSCORE bit.
                    attributes |= COMMON_LVB_UNDERSCORE;
                    ++i;
                    continue;
                }

                if (param == 24)
                {
                    // Clear underline.
                    attributes = static_cast<USHORT>(attributes & ~COMMON_LVB_UNDERSCORE);
                    ++i;
                    continue;
                }

                if (param == 7)
                {
                    // "Negative" / reverse video.
                    attributes |= COMMON_LVB_REVERSE_VIDEO;
                    ++i;
                    continue;
                }

                if (param == 27)
                {
                    // Clear reverse video.
                    attributes = static_cast<USHORT>(attributes & ~COMMON_LVB_REVERSE_VIDEO);
                    ++i;
                    continue;
                }

                if (param == 39)
                {
                    // Default foreground color.
                    attributes = static_cast<USHORT>((attributes & ~fg_full_mask) | (default_attributes & fg_full_mask));
                    ++i;
                    continue;
                }

                if (param == 49)
                {
                    // Default background color.
                    attributes = static_cast<USHORT>((attributes & ~bg_full_mask) | (default_attributes & bg_full_mask));
                    ++i;
                    continue;
                }

                if (param >= 30 && param <= 37)
                {
                    set_foreground(param - 30, false);
                    ++i;
                    continue;
                }

                if (param >= 90 && param <= 97)
                {
                    set_foreground(param - 90, true);
                    ++i;
                    continue;
                }

                if (param >= 40 && param <= 47)
                {
                    set_background(param - 40, false);
                    ++i;
                    continue;
                }

                if (param >= 100 && param <= 107)
                {
                    set_background(param - 100, true);
                    ++i;
                    continue;
                }

                if (param == 38 || param == 48)
                {
                    const bool foreground = (param == 38);
                    if (i + 1 < csi.param_count)
                    {
                        const unsigned mode = csi.params[i + 1];
                        if (mode == 5 && i + 2 < csi.param_count)
                        {
                            unsigned index = clamp_byte(csi.params[i + 2]);
                            if (index < 16)
                            {
                                // For the base palette, mimic classic SGR semantics (30-37/90-97).
                                const unsigned base = index & 0x07U;
                                const bool bright = (index & 0x08U) != 0;
                                if (foreground)
                                {
                                    set_foreground(base, bright);
                                }
                                else
                                {
                                    set_background(base, bright);
                                }
                            }
                            else
                            {
                                unsigned red{};
                                unsigned green{};
                                unsigned blue{};
                                xterm_256_index_to_rgb(index, red, green, blue);
                                const unsigned nearest = nearest_palette_index(red, green, blue);
                                set_palette_index(nearest, foreground);
                            }

                            i += 3;
                            continue;
                        }

                        if (mode == 2 && i + 4 < csi.param_count)
                        {
                            const unsigned red = clamp_byte(csi.params[i + 2]);
                            const unsigned green = clamp_byte(csi.params[i + 3]);
                            const unsigned blue = clamp_byte(csi.params[i + 4]);
                            const unsigned nearest = nearest_palette_index(red, green, blue);
                            set_palette_index(nearest, foreground);
                            i += 5;
                            continue;
                        }
                    }
                }

                // Ignore unsupported SGR parameters.
                ++i;
            }
        };



        const auto apply_csi = [&](const auto& csi) noexcept {
                    // In VT processing mode, CSI sequences are not printed to the buffer.
                    // Only a minimal subset is applied to the buffer model.
                    if (csi.final == L'm')
                    {
                        apply_sgr(csi);
                    }
                    else if (csi.final == L'n')
                    {
                        // DSR: Device Status Report.
                        // Minimal support:
                        // - CSI 5 n: "operating status" -> CSI 0 n
                        // - CSI 6 n: cursor position report -> CSI row ; col R
                        const unsigned id = csi.param_count >= 1 ? csi.params[0] : 0U;
                        if (host_io != nullptr &&
                            host_io->vt_should_answer_queries() &&
                            (id == 5U || id == 6U))
                        {
                            std::array<char, 32> response{};
                            size_t pos = 0;

                            const auto append = [&](const std::string_view bytes) noexcept {
                                if (bytes.empty())
                                {
                                    return;
                                }

                                const size_t remaining = response.size() - pos;
                                const size_t to_copy = std::min(remaining, bytes.size());
                                if (to_copy != 0)
                                {
                                    std::memcpy(response.data() + pos, bytes.data(), to_copy);
                                    pos += to_copy;
                                }
                            };

                            const auto append_number = [&](unsigned value) noexcept {
                                const size_t remaining = response.size() - pos;
                                if (remaining == 0)
                                {
                                    return;
                                }

                                auto [ptr, ec] = std::to_chars(response.data() + pos, response.data() + response.size(), value);
                                if (ec == std::errc{})
                                {
                                    pos += static_cast<size_t>(ptr - (response.data() + pos));
                                }
                            };

                            if (id == 5U)
                            {
                                append("\x1b[0n");
                            }
                            else
                            {
                                // Report the cursor position relative to the visible window (1-based).
                                // When origin mode is enabled, the row is relative to the active DECSTBM top margin.
                                const auto window = screen_buffer.window_rect();
                                const auto [top, bottom] = resolve_vertical_region();
                                (void)bottom;
                                const long y_origin = vt_origin_mode ? static_cast<long>(top) : static_cast<long>(window.Top);
                                const long x_origin = static_cast<long>(window.Left);

                                long row = static_cast<long>(cursor.Y) - y_origin + 1;
                                long col = static_cast<long>(cursor.X) - x_origin + 1;
                                if (row < 1)
                                {
                                    row = 1;
                                }
                                if (col < 1)
                                {
                                    col = 1;
                                }

                                append("\x1b[");
                                if (csi.private_marker)
                                {
                                    append("?");
                                }
                                append_number(static_cast<unsigned>(row));
                                append(";");
                                append_number(static_cast<unsigned>(col));
                                if (csi.private_marker)
                                {
                                    // The extended report includes a page number. The replacement has no
                                    // page concept, so we report a single default page.
                                    append(";1");
                                }
                                append("R");
                            }

                            if (pos != 0)
                            {
                                (void)host_io->inject_input_bytes(std::as_bytes(std::span<const char>(response.data(), pos)));
                            }
                        }
                    }
                    else if (csi.final == L'H' || csi.final == L'f')
                    {
                        const unsigned row = csi.param_count >= 1 ? csi.params[0] : 1U;
                        const unsigned col = csi.param_count >= 2 ? csi.params[1] : 1U;
                        const unsigned row_value = row == 0 ? 1U : row;
                        const unsigned col_value = col == 0 ? 1U : col;

                        const auto [top, bottom] = resolve_vertical_region();
                        const long y_offset = vt_origin_mode ? static_cast<long>(top) : 0;
                        const long y_min = vt_origin_mode ? static_cast<long>(top) : 0;
                        const long y_max = vt_origin_mode ? static_cast<long>(bottom) : static_cast<long>(buffer_size.Y - 1);

                        const long new_y = static_cast<long>(row_value - 1U) + y_offset;
                        const long new_x = static_cast<long>(col_value - 1U);

                        cursor.X = static_cast<SHORT>(std::clamp(new_x, 0L, static_cast<long>(buffer_size.X - 1)));
                        cursor.Y = static_cast<SHORT>(std::clamp(new_y, y_min, y_max));
                    }
                    else if (csi.final == L'p' && csi.exclamation_marker)
                    {
                        // DECSTR: Soft reset (CSI ! p).
                        //
                        // This resets a subset of VT state without clearing the screen. It is used by
                        // TUIs that want a known mode baseline while preserving the buffer contents.
                        screen_buffer.set_cursor_info(screen_buffer.cursor_size(), true);

                        vt_autowrap = true;
                        vt_origin_mode = false;
                        vt_insert_mode = false;
                        vt_delayed_wrap_position.reset();

                        vt_vertical_margins.reset();
                        screen_buffer.set_vt_vertical_margins(std::nullopt);

                        attributes = default_attributes;
                        screen_buffer.save_cursor_state(COORD{ 0, 0 }, attributes, false, false);
                    }
                    else if (csi.final == L'G' || csi.final == L'`')
                    {
                        // CHA/HPA: Cursor Horizontal Absolute.
                        const unsigned col = csi.param_count >= 1 ? csi.params[0] : 1U;
                        const unsigned col_value = col == 0 ? 1U : col;
                        const long new_x = static_cast<long>(col_value - 1U);
                        cursor.X = static_cast<SHORT>(std::clamp(new_x, 0L, static_cast<long>(buffer_size.X - 1)));
                    }
                    else if (csi.final == L'd')
                    {
                        // VPA: Vertical Position Absolute.
                        const unsigned row = csi.param_count >= 1 ? csi.params[0] : 1U;
                        const unsigned row_value = row == 0 ? 1U : row;

                        const auto [top, bottom] = resolve_vertical_region();
                        const long y_offset = vt_origin_mode ? static_cast<long>(top) : 0;
                        const long y_min = vt_origin_mode ? static_cast<long>(top) : 0;
                        const long y_max = vt_origin_mode ? static_cast<long>(bottom) : static_cast<long>(buffer_size.Y - 1);

                        const long new_y = static_cast<long>(row_value - 1U) + y_offset;
                        cursor.Y = static_cast<SHORT>(std::clamp(new_y, y_min, y_max));
                    }
                    else if (csi.final == L'E' || csi.final == L'F')
                    {
                        // CNL/CPL: Cursor Next/Previous Line.
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        const long delta = (csi.final == L'E') ? static_cast<long>(count) : -static_cast<long>(count);
                        const long new_y = static_cast<long>(cursor.Y) + delta;

                        const auto [top, bottom] = resolve_vertical_region();
                        const bool clamp_to_margins =
                            vt_origin_mode ||
                            (vt_vertical_margins && cursor.Y >= top && cursor.Y <= bottom);
                        const long y_min = clamp_to_margins ? static_cast<long>(top) : 0;
                        const long y_max = clamp_to_margins ? static_cast<long>(bottom) : static_cast<long>(buffer_size.Y - 1);

                        cursor.X = 0;
                        cursor.Y = static_cast<SHORT>(std::clamp(new_y, y_min, y_max));
                    }
                    else if (csi.final == L'A' || csi.final == L'B' || csi.final == L'C' || csi.final == L'D')
                    {
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        long new_x = cursor.X;
                        long new_y = cursor.Y;
                        switch (csi.final)
                        {
                        case L'A':
                            new_y -= static_cast<long>(count);
                            break;
                        case L'B':
                            new_y += static_cast<long>(count);
                            break;
                        case L'C':
                            new_x += static_cast<long>(count);
                            break;
                        case L'D':
                            new_x -= static_cast<long>(count);
                            break;
                        default:
                            break;
                        }

                        const auto [top, bottom] = resolve_vertical_region();
                        const bool clamp_to_margins =
                            vt_origin_mode ||
                            (vt_vertical_margins && cursor.Y >= top && cursor.Y <= bottom);
                        const long y_min = clamp_to_margins ? static_cast<long>(top) : 0;
                        const long y_max = clamp_to_margins ? static_cast<long>(bottom) : static_cast<long>(buffer_size.Y - 1);

                        cursor.X = static_cast<SHORT>(std::clamp(new_x, 0L, static_cast<long>(buffer_size.X - 1)));
                        cursor.Y = static_cast<SHORT>(std::clamp(new_y, y_min, y_max));
                    }
                    else if (csi.final == L'@')
                    {
                        // ICH: Insert Character (blank cells) at the current cursor position.
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        const long width_long = buffer_size.X;
                        const long x0 = cursor.X;
                        const long y = cursor.Y;
                        if (width_long > 0 && x0 >= 0 && y >= 0 && y < buffer_size.Y && x0 < width_long)
                        {
                            const unsigned remaining = static_cast<unsigned>(width_long - x0);
                            if (count > remaining)
                            {
                                count = remaining;
                            }

                            wchar_t src_char{};
                            USHORT src_attr{};
                            for (long x = width_long - 1; x >= x0 + static_cast<long>(count); --x)
                            {
                                if (screen_buffer.read_output_characters(COORD{ static_cast<SHORT>(x - static_cast<long>(count)), static_cast<SHORT>(y) }, std::span<wchar_t>(&src_char, 1)) != 1 ||
                                    screen_buffer.read_output_attributes(COORD{ static_cast<SHORT>(x - static_cast<long>(count)), static_cast<SHORT>(y) }, std::span<USHORT>(&src_attr, 1)) != 1)
                                {
                                    src_char = L' ';
                                    src_attr = attributes;
                                }

                                (void)screen_buffer.write_cell(COORD{ static_cast<SHORT>(x), static_cast<SHORT>(y) }, src_char, src_attr);
                            }

                            for (unsigned i = 0; i < count; ++i)
                            {
                                (void)screen_buffer.write_cell(
                                    COORD{ static_cast<SHORT>(x0 + static_cast<long>(i)), static_cast<SHORT>(y) },
                                    L' ',
                                    attributes);
                            }
                        }

                        // ICH resets the delayed wrap flag (the "last column flag").
                        vt_delayed_wrap_position.reset();
                    }
                    else if (csi.final == L'P')
                    {
                        // DCH: Delete Character(s) at the current cursor position (shifts the line left).
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        const long width_long = buffer_size.X;
                        const long x0 = cursor.X;
                        const long y = cursor.Y;
                        if (width_long > 0 && x0 >= 0 && y >= 0 && y < buffer_size.Y && x0 < width_long)
                        {
                            const unsigned remaining = static_cast<unsigned>(width_long - x0);
                            if (count > remaining)
                            {
                                count = remaining;
                            }

                            const long limit = width_long - static_cast<long>(count);
                            wchar_t src_char{};
                            USHORT src_attr{};
                            for (long x = x0; x < limit; ++x)
                            {
                                if (screen_buffer.read_output_characters(COORD{ static_cast<SHORT>(x + static_cast<long>(count)), static_cast<SHORT>(y) }, std::span<wchar_t>(&src_char, 1)) != 1 ||
                                    screen_buffer.read_output_attributes(COORD{ static_cast<SHORT>(x + static_cast<long>(count)), static_cast<SHORT>(y) }, std::span<USHORT>(&src_attr, 1)) != 1)
                                {
                                    src_char = L' ';
                                    src_attr = attributes;
                                }

                                (void)screen_buffer.write_cell(COORD{ static_cast<SHORT>(x), static_cast<SHORT>(y) }, src_char, src_attr);
                            }

                            for (long x = limit; x < width_long; ++x)
                            {
                                (void)screen_buffer.write_cell(COORD{ static_cast<SHORT>(x), static_cast<SHORT>(y) }, L' ', attributes);
                            }
                        }

                        // DCH resets the delayed wrap flag.
                        vt_delayed_wrap_position.reset();
                    }
                    else if (csi.final == L'X')
                    {
                        // ECH: Erase Character(s) from the current cursor position (replaces with space).
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        const long width_long = buffer_size.X;
                        const long x0 = cursor.X;
                        const long y = cursor.Y;
                        if (width_long > 0 && x0 >= 0 && y >= 0 && y < buffer_size.Y && x0 < width_long)
                        {
                            const unsigned remaining = static_cast<unsigned>(width_long - x0);
                            if (count > remaining)
                            {
                                count = remaining;
                            }

                            for (unsigned i = 0; i < count; ++i)
                            {
                                (void)screen_buffer.write_cell(
                                    COORD{ static_cast<SHORT>(x0 + static_cast<long>(i)), static_cast<SHORT>(y) },
                                    L' ',
                                    attributes);
                            }
                        }

                        // ECH resets the delayed wrap flag.
                        vt_delayed_wrap_position.reset();
                    }
                    else if (csi.final == L'r')
                    {
                        // DECSTBM: Set top/bottom scrolling margins.
                        // Parameters are 1-based and default to 1 and the page height.
                        const unsigned requested_top = csi.param_count >= 1 ? csi.params[0] : 0U;
                        const unsigned requested_bottom = csi.param_count >= 2 ? csi.params[1] : 0U;

                        const unsigned page_height = static_cast<unsigned>(buffer_size.Y);
                        unsigned actual_top = requested_top == 0 ? 1U : requested_top;
                        unsigned actual_bottom = requested_bottom == 0 ? page_height : requested_bottom;

                        if (actual_top < actual_bottom && actual_bottom <= page_height)
                        {
                            if (actual_top == 1U && actual_bottom == page_height)
                            {
                                vt_vertical_margins.reset();
                                screen_buffer.set_vt_vertical_margins(std::nullopt);
                            }
                            else
                            {
                                ScreenBuffer::VtVerticalMargins margins{};
                                margins.top = static_cast<SHORT>(actual_top - 1U);
                                margins.bottom = static_cast<SHORT>(actual_bottom - 1U);
                                vt_vertical_margins = margins;
                                screen_buffer.set_vt_vertical_margins(vt_vertical_margins);
                            }

                            // Conhost homes the cursor on valid DECSTBM updates.
                            const auto [top, bottom] = resolve_vertical_region();
                            (void)bottom;
                            cursor = COORD{ 0, vt_origin_mode ? top : static_cast<SHORT>(0) };
                        }
                    }
                    else if (csi.final == L'S' || csi.final == L'T')
                    {
                        // SU/SD: Scroll Up/Down within the current DECSTBM margins.
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        const auto [top, bottom] = resolve_vertical_region();
                        if (csi.final == L'S')
                        {
                            scroll_region_up(top, bottom, count);
                        }
                        else
                        {
                            scroll_region_down(top, bottom, count);
                        }
                    }
                    else if (csi.final == L'L')
                    {
                        // IL: Insert line(s) at the cursor row (within the scrolling margins).
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        const auto [top, bottom] = resolve_vertical_region();
                        if (cursor.Y >= top && cursor.Y <= bottom)
                        {
                            const unsigned region_height =
                                static_cast<unsigned>(static_cast<long>(bottom) - static_cast<long>(cursor.Y) + 1);
                            if (count >= region_height)
                            {
                                const size_t width = static_cast<size_t>(buffer_size.X);
                                const size_t height = static_cast<size_t>(region_height);
                                const size_t length = width * height;
                                const COORD origin{ 0, cursor.Y };
                                (void)screen_buffer.fill_output_characters(origin, L' ', length);
                                (void)screen_buffer.fill_output_attributes(origin, attributes, length);
                            }
                            else
                            {
                                const auto right = static_cast<SHORT>(buffer_size.X - 1);
                                const auto src_bottom =
                                    static_cast<SHORT>(static_cast<long>(bottom) - static_cast<long>(count));
                                SMALL_RECT scroll_rect{};
                                scroll_rect.Left = 0;
                                scroll_rect.Top = cursor.Y;
                                scroll_rect.Right = right;
                                scroll_rect.Bottom = src_bottom;

                                SMALL_RECT clip_rect{};
                                clip_rect.Left = 0;
                                clip_rect.Top = cursor.Y;
                                clip_rect.Right = right;
                                clip_rect.Bottom = bottom;

                                const auto dest_top =
                                    static_cast<SHORT>(static_cast<long>(cursor.Y) + static_cast<long>(count));
                                (void)screen_buffer.scroll_screen_buffer(
                                    scroll_rect,
                                    clip_rect,
                                    COORD{ 0, dest_top },
                                    L' ',
                                    attributes);
                            }
                        }
                    }
                    else if (csi.final == L'M')
                    {
                        // DL: Delete line(s) at the cursor row (within the scrolling margins).
                        unsigned count = csi.param_count >= 1 ? csi.params[0] : 1U;
                        if (count == 0)
                        {
                            count = 1;
                        }

                        const auto [top, bottom] = resolve_vertical_region();
                        if (cursor.Y >= top && cursor.Y <= bottom)
                        {
                            const unsigned region_height =
                                static_cast<unsigned>(static_cast<long>(bottom) - static_cast<long>(cursor.Y) + 1);
                            if (count >= region_height)
                            {
                                const size_t width = static_cast<size_t>(buffer_size.X);
                                const size_t height = static_cast<size_t>(region_height);
                                const size_t length = width * height;
                                const COORD origin{ 0, cursor.Y };
                                (void)screen_buffer.fill_output_characters(origin, L' ', length);
                                (void)screen_buffer.fill_output_attributes(origin, attributes, length);
                            }
                            else
                            {
                                const auto right = static_cast<SHORT>(buffer_size.X - 1);
                                const auto src_top =
                                    static_cast<SHORT>(static_cast<long>(cursor.Y) + static_cast<long>(count));
                                SMALL_RECT scroll_rect{};
                                scroll_rect.Left = 0;
                                scroll_rect.Top = src_top;
                                scroll_rect.Right = right;
                                scroll_rect.Bottom = bottom;

                                SMALL_RECT clip_rect{};
                                clip_rect.Left = 0;
                                clip_rect.Top = cursor.Y;
                                clip_rect.Right = right;
                                clip_rect.Bottom = bottom;

                                (void)screen_buffer.scroll_screen_buffer(
                                    scroll_rect,
                                    clip_rect,
                                    COORD{ 0, cursor.Y },
                                    L' ',
                                    attributes);
                            }
                        }
                    }
                    else if (csi.final == L'J')
                    {
                        const unsigned mode = csi.param_count >= 1 ? csi.params[0] : 0U;
                        const size_t width = static_cast<size_t>(buffer_size.X);
                        const size_t height = static_cast<size_t>(buffer_size.Y);
                        const size_t total_cells = width * height;
                        const size_t cursor_index = static_cast<size_t>(cursor.Y) * width + static_cast<size_t>(cursor.X);

                        COORD origin{};
                        size_t length = 0;
                        switch (mode)
                        {
                        case 0: // cursor -> end
                            origin = cursor;
                            length = cursor_index < total_cells ? total_cells - cursor_index : 0;
                            break;
                        case 1: // start -> cursor
                            origin = COORD{ 0, 0 };
                            length = cursor_index < total_cells ? cursor_index + 1 : total_cells;
                            break;
                        case 2: // entire screen
                        case 3: // entire screen + scrollback (not modeled separately yet)
                            origin = COORD{ 0, 0 };
                            length = total_cells;
                            break;
                        default:
                            break;
                        }

                        if (length != 0)
                        {
                            (void)screen_buffer.fill_output_characters(origin, L' ', length);
                            (void)screen_buffer.fill_output_attributes(origin, attributes, length);
                        }

                        // ED resets the delayed wrap flag.
                        vt_delayed_wrap_position.reset();
                    }
                    else if (csi.final == L'K')
                    {
                        const unsigned mode = csi.param_count >= 1 ? csi.params[0] : 0U;
                        const long width_long = buffer_size.X;
                        if (width_long > 0)
                        {
                            const size_t width = static_cast<size_t>(width_long);

                            COORD origin{};
                            size_t length = 0;
                            switch (mode)
                            {
                            case 0: // cursor -> end of line
                                origin = cursor;
                                length = cursor.X < buffer_size.X ? width - static_cast<size_t>(cursor.X) : 0;
                                break;
                            case 1: // start of line -> cursor
                                origin = COORD{ 0, cursor.Y };
                                length = cursor.X < buffer_size.X ? static_cast<size_t>(cursor.X) + 1 : width;
                                break;
                            case 2: // entire line
                                origin = COORD{ 0, cursor.Y };
                                length = width;
                                break;
                            default:
                                break;
                            }

                            if (length != 0)
                            {
                                (void)screen_buffer.fill_output_characters(origin, L' ', length);
                                (void)screen_buffer.fill_output_attributes(origin, attributes, length);
                            }
                        }

                        // EL resets the delayed wrap flag.
                        vt_delayed_wrap_position.reset();
                    }
                    else if (csi.final == L's')
                    {
                        // `ESC[s` is ambiguous (DECSLRM vs ANSISYSSC) in the upstream parser and
                        // depends on DECLRMM state. For the in-memory model we treat a no-parameter
                        // `s` sequence as Save Cursor.
                        if (csi.param_count == 0)
                        {
                            const bool delayed_eol_wrap =
                                vt_delayed_wrap_position.has_value() &&
                                vt_delayed_wrap_position->X == cursor.X &&
                                vt_delayed_wrap_position->Y == cursor.Y;
                            screen_buffer.save_cursor_state(cursor, attributes, delayed_eol_wrap, vt_origin_mode);
                        }
                    }
                    else if (csi.final == L'u')
                    {
                        // `ESC[u` restores the last saved cursor state.
                        COORD restored{};
                        USHORT restored_attributes{};
                        bool delayed_eol_wrap = false;
                        bool origin_mode_enabled = false;
                        if (screen_buffer.restore_cursor_state(restored, restored_attributes, delayed_eol_wrap, origin_mode_enabled))
                        {
                            cursor = restored;
                            attributes = restored_attributes;
                            vt_origin_mode = origin_mode_enabled;

                            cursor.X = static_cast<SHORT>(std::clamp(static_cast<long>(cursor.X), 0L, static_cast<long>(buffer_size.X - 1)));
                            const auto [top, bottom] = resolve_vertical_region();
                            const long y_min = vt_origin_mode ? static_cast<long>(top) : 0;
                            const long y_max = vt_origin_mode ? static_cast<long>(bottom) : static_cast<long>(buffer_size.Y - 1);
                            cursor.Y = static_cast<SHORT>(std::clamp(static_cast<long>(cursor.Y), y_min, y_max));

                            vt_delayed_wrap_position = delayed_eol_wrap ? std::optional<COORD>(cursor) : std::nullopt;
                        }
                    }
                    else if (csi.final == L'h' || csi.final == L'l')
                    {
                        // Handle a minimal subset of mode toggles used by console clients:
                        // - IRM (ANSI Standard Mode 4): insert/replace mode (CSI 4 h/l).
                        // - DECTCEM (DEC Private Mode 25): text cursor enable/disable (CSI ? 25 h/l).
                        // - DECOM (DEC Private Mode 6): origin mode (CSI ? 6 h/l).
                        // - DECAWM (DEC Private Mode 7): autowrap enable/disable (CSI ? 7 h/l).
                        // - Alternate Screen Buffer (DEC Private Mode 1049): (CSI ? 1049 h/l).
                        //
                        // For DEC private modes the upstream parser requires the `?` marker.
                        // Our CSI parser records it as `private_marker`, but we intentionally key
                        // off parameter values for this minimal subset. IRM is applied only when
                        // the marker is absent to avoid consuming unrelated DEC private modes.
                        const bool enable = (csi.final == L'h');
                        for (size_t i = 0; i < csi.param_count; ++i)
                        {
                            const unsigned param = csi.params[i];
                            if (!csi.private_marker && param == 4U)
                            {
                                vt_insert_mode = enable;
                            }
                            else if (param == 25U)
                            {
                                screen_buffer.set_cursor_info(screen_buffer.cursor_size(), enable);
                            }
                            else if (param == 6U)
                            {
                                vt_origin_mode = enable;
                                const auto [top, bottom] = resolve_vertical_region();
                                (void)bottom;
                                cursor = COORD{ 0, vt_origin_mode ? top : static_cast<SHORT>(0) };
                                vt_delayed_wrap_position.reset();
                            }
                            else if (param == 7U)
                            {
                                vt_autowrap = enable;
                                vt_delayed_wrap_position.reset();
                            }
                            else if (param == 1049U)
                            {
                                if (screen_buffer.set_vt_using_alternate_screen_buffer(enable, L' ', attributes))
                                {
                                    cursor = screen_buffer.cursor_position();
                                    attributes = screen_buffer.text_attributes();
                                    vt_vertical_margins = screen_buffer.vt_vertical_margins();
                                    vt_origin_mode = screen_buffer.vt_origin_mode_enabled();
                                    vt_delayed_wrap_position.reset();
                                }
                            }
                        }
                    }

        };
        for (size_t offset = 0; offset < text.size();)
        {
            const wchar_t ch = text[offset];
            if (vt_processing)
            {
                auto& vt_state = screen_buffer._vt_output_parse_state;
                using Phase = detail::VtOutputParseState::Phase;

                switch (vt_state.phase)
                {
                case Phase::escape:
                {
                    const wchar_t esc_final = ch;
                    vt_state.phase = Phase::ground;
                    vt_state.esc_intermediate_count = 0;
                    vt_state.esc_length = 0;

                    // CSI introducer.
                    if (esc_final == L'[')
                    {
                        vt_state.phase = Phase::csi;
                        vt_state.csi = {};
                        vt_state.csi_current = 0;
                        vt_state.csi_have_digits = false;
                        vt_state.csi_last_was_separator = false;
                        vt_state.csi_length = 0;
                        ++offset;
                        continue;
                    }

                    // OSC introducer.
                    if (esc_final == L']')
                    {
                        vt_state.phase = Phase::osc;
                        vt_state.osc_param = 0;
                        vt_state.osc_param_have_digits = false;
                        vt_state.osc_in_param = true;
                        vt_state.osc_action = 0;
                        vt_state.osc_capture_payload = false;
                        vt_state.osc_payload_length = 0;
                        ++offset;
                        continue;
                    }

                    // DCS/PM/APC/SOS string introducers (payload is ignored until ST).
                    if (esc_final == L'P' || esc_final == L'^' || esc_final == L'_' || esc_final == L'X')
                    {
                        vt_state.phase = Phase::string;
                        ++offset;
                        continue;
                    }

                    // DECSC/DECRC: ESC7 / ESC8.
                    if (esc_final == L'7')
                    {
                        const bool delayed_eol_wrap =
                            vt_delayed_wrap_position.has_value() &&
                            vt_delayed_wrap_position->X == cursor.X &&
                            vt_delayed_wrap_position->Y == cursor.Y;
                        screen_buffer.save_cursor_state(cursor, attributes, delayed_eol_wrap, vt_origin_mode);
                        ++offset;
                        continue;
                    }

                    if (esc_final == L'8')
                    {
                        COORD restored{};
                        USHORT restored_attributes{};
                        bool delayed_eol_wrap = false;
                        bool origin_mode_enabled = false;
                        if (screen_buffer.restore_cursor_state(restored, restored_attributes, delayed_eol_wrap, origin_mode_enabled))
                        {
                            cursor = restored;
                            attributes = restored_attributes;
                            vt_origin_mode = origin_mode_enabled;

                            cursor.X = static_cast<SHORT>(std::clamp(static_cast<long>(cursor.X), 0L, static_cast<long>(buffer_size.X - 1)));
                            const auto [top, bottom] = resolve_vertical_region();
                            const long y_min = vt_origin_mode ? static_cast<long>(top) : 0;
                            const long y_max = vt_origin_mode ? static_cast<long>(bottom) : static_cast<long>(buffer_size.Y - 1);
                            cursor.Y = static_cast<SHORT>(std::clamp(static_cast<long>(cursor.Y), y_min, y_max));

                            vt_delayed_wrap_position = delayed_eol_wrap ? std::optional<COORD>(cursor) : std::nullopt;
                        }
                        ++offset;
                        continue;
                    }

                    // Index / Reverse Index: ESC D / ESC M.
                    if (esc_final == L'D')
                    {
                        line_feed();
                        ++offset;
                        continue;
                    }

                    if (esc_final == L'M')
                    {
                        reverse_line_feed();
                        ++offset;
                        continue;
                    }

                    // NEL: Next Line (ESC E).
                    if (esc_final == L'E')
                    {
                        cursor.X = 0;
                        line_feed();
                        ++offset;
                        continue;
                    }

                    if (esc_final == L'c')
                    {
                        // RIS: Hard reset (ESC c).
                        if (screen_buffer.vt_using_alternate_screen_buffer())
                        {
                            (void)screen_buffer.set_vt_using_alternate_screen_buffer(false, L' ', attributes);
                            cursor = screen_buffer.cursor_position();
                            attributes = screen_buffer.text_attributes();
                            vt_vertical_margins = screen_buffer.vt_vertical_margins();
                            vt_origin_mode = screen_buffer.vt_origin_mode_enabled();
                            vt_insert_mode = screen_buffer.vt_insert_mode_enabled();
                            vt_delayed_wrap_position = screen_buffer.vt_delayed_wrap_position();
                        }

                        const auto defaults = ScreenBuffer::default_settings();
                        COLORREF table[16]{};
                        for (size_t i = 0; i < defaults.color_table.size(); ++i)
                        {
                            table[i] = defaults.color_table[i];
                        }
                        screen_buffer.set_color_table(table);

                        screen_buffer.set_cursor_info(screen_buffer.cursor_size(), true);
                        screen_buffer.save_cursor_state(COORD{ 0, 0 }, default_attributes, false, false);

                        vt_autowrap = true;
                        vt_origin_mode = false;
                        vt_insert_mode = false;
                        vt_delayed_wrap_position.reset();

                        vt_vertical_margins.reset();
                        screen_buffer.set_vt_vertical_margins(std::nullopt);

                        attributes = default_attributes;
                        cursor = COORD{ 0, 0 };

                        const size_t length = static_cast<size_t>(buffer_size.X) * static_cast<size_t>(buffer_size.Y);
                        (void)screen_buffer.fill_output_characters(cursor, L' ', length);
                        (void)screen_buffer.fill_output_attributes(cursor, attributes, length);

                        ++offset;
                        continue;
                    }

                    // String terminator (ESC \\) is a no-op when written directly.
                    if (esc_final == L'\\')
                    {
                        ++offset;
                        continue;
                    }

                    // ESC dispatch with intermediates (charset designation, DECALN, etc).
                    if (esc_final >= 0x20 && esc_final <= 0x2f)
                    {
                        vt_state.phase = Phase::esc_dispatch;
                        vt_state.esc_intermediate_count = 0;
                        vt_state.esc_length = 1;
                        if (!vt_state.esc_intermediates.empty())
                        {
                            vt_state.esc_intermediates[0] = esc_final;
                            vt_state.esc_intermediate_count = 1;
                        }
                        ++offset;
                        continue;
                    }

                    // Consume unsupported ESC dispatch finals as no-ops to avoid escape-byte leakage.
                    ++offset;
                    continue;
                }
                case Phase::esc_dispatch:
                {
                    constexpr size_t max_sequence_length = 16;
                    const wchar_t candidate = ch;

                    if (candidate >= 0x20 && candidate <= 0x2f)
                    {
                        if (vt_state.esc_intermediate_count < vt_state.esc_intermediates.size())
                        {
                            vt_state.esc_intermediates[vt_state.esc_intermediate_count++] = candidate;
                        }

                        if (++vt_state.esc_length >= max_sequence_length)
                        {
                            vt_state.phase = Phase::ground;
                            vt_state.esc_intermediate_count = 0;
                            vt_state.esc_length = 0;
                        }

                        ++offset;
                        continue;
                    }

                    if (candidate >= 0x30 && candidate <= 0x7e)
                    {
                        // DECALN: Screen alignment pattern (ESC # 8).
                        if (vt_state.esc_intermediate_count == 1 &&
                            vt_state.esc_intermediates[0] == L'#' &&
                            candidate == L'8')
                        {
                            vt_delayed_wrap_position.reset();

                            const size_t length =
                                static_cast<size_t>(buffer_size.X) *
                                static_cast<size_t>(buffer_size.Y);
                            (void)screen_buffer.fill_output_characters(COORD{ 0, 0 }, L'E', length);
                            (void)screen_buffer.fill_output_attributes(COORD{ 0, 0 }, default_attributes, length);

                            attributes = static_cast<USHORT>(attributes & ~(COMMON_LVB_REVERSE_VIDEO | COMMON_LVB_UNDERSCORE));

                            vt_origin_mode = false;
                            vt_vertical_margins.reset();
                            screen_buffer.set_vt_vertical_margins(std::nullopt);
                            cursor = COORD{ 0, 0 };
                        }

                        vt_state.phase = Phase::ground;
                        vt_state.esc_intermediate_count = 0;
                        vt_state.esc_length = 0;
                        ++offset;
                        continue;
                    }

                    vt_state.phase = Phase::ground;
                    vt_state.esc_intermediate_count = 0;
                    vt_state.esc_length = 0;
                    ++offset;
                    continue;
                }
                case Phase::csi:
                {
                    constexpr size_t max_sequence_length = 128;
                    if (vt_state.csi_length++ >= max_sequence_length)
                    {
                        vt_state.phase = Phase::ground;
                        vt_state.csi = {};
                        vt_state.csi_current = 0;
                        vt_state.csi_have_digits = false;
                        vt_state.csi_last_was_separator = false;
                        vt_state.csi_length = 0;
                        ++offset;
                        continue;
                    }

                    if (ch >= L'0' && ch <= L'9')
                    {
                        vt_state.csi_have_digits = true;
                        vt_state.csi_last_was_separator = false;
                        const unsigned digit = static_cast<unsigned>(ch - L'0');
                        if (vt_state.csi_current <= 1'000'000U)
                        {
                            vt_state.csi_current = vt_state.csi_current * 10U + digit;
                        }
                        ++offset;
                        continue;
                    }

                    if (ch == L'?')
                    {
                        vt_state.csi.private_marker = true;
                        ++offset;
                        continue;
                    }

                    if (ch == L'!')
                    {
                        vt_state.csi.exclamation_marker = true;
                        ++offset;
                        continue;
                    }

                    if (ch == L';')
                    {
                        vt_state.csi_last_was_separator = true;
                        if (vt_state.csi.param_count < vt_state.csi.params.size())
                        {
                            vt_state.csi.params[vt_state.csi.param_count++] = vt_state.csi_have_digits ? vt_state.csi_current : 0U;
                        }
                        vt_state.csi_current = 0;
                        vt_state.csi_have_digits = false;
                        ++offset;
                        continue;
                    }

                    if (ch >= 0x40 && ch <= 0x7e)
                    {
                        if (vt_state.csi_have_digits || vt_state.csi_last_was_separator)
                        {
                            if (vt_state.csi.param_count < vt_state.csi.params.size())
                            {
                                vt_state.csi.params[vt_state.csi.param_count++] = vt_state.csi_have_digits ? vt_state.csi_current : 0U;
                            }
                        }

                        vt_state.csi.final = ch;
                        if (vt_state.csi.final == L'm' && vt_state.csi.param_count == 0)
                        {
                            vt_state.csi.params[vt_state.csi.param_count++] = 0U;
                        }

                        apply_csi(vt_state.csi);

                        vt_state.phase = Phase::ground;
                        vt_state.csi = {};
                        vt_state.csi_current = 0;
                        vt_state.csi_have_digits = false;
                        vt_state.csi_last_was_separator = false;
                        vt_state.csi_length = 0;
                        ++offset;
                        continue;
                    }

                    // Ignore intermediate/private parameter bytes while waiting for the final byte.
                    ++offset;
                    continue;
                }
                case Phase::osc:
                {
                    if (vt_state.osc_in_param)
                    {
                        if (ch >= L'0' && ch <= L'9')
                        {
                            vt_state.osc_param_have_digits = true;
                            const unsigned digit = static_cast<unsigned>(ch - L'0');
                            if (vt_state.osc_param <= 1'000'000U)
                            {
                                vt_state.osc_param = vt_state.osc_param * 10U + digit;
                            }
                            ++offset;
                            continue;
                        }

                        if (ch == L';')
                        {
                            vt_state.osc_action = vt_state.osc_param_have_digits ? vt_state.osc_param : 0U;
                            const unsigned action = vt_state.osc_action;
                            vt_state.osc_capture_payload = (action == 0U || action == 1U || action == 2U || action == 21U);
                            vt_state.osc_in_param = false;
                            vt_state.osc_payload_length = 0;
                            ++offset;
                            continue;
                        }

                        // Invalid OSC parameter bytes: abort and return to ground.
                        vt_state.phase = Phase::ground;
                        vt_state.osc_param = 0;
                        vt_state.osc_param_have_digits = false;
                        vt_state.osc_in_param = true;
                        vt_state.osc_action = 0;
                        vt_state.osc_capture_payload = false;
                        vt_state.osc_payload_length = 0;
                        ++offset;
                        continue;
                    }

                    if (ch == L'\x07' || ch == L'\x9c')
                    {
                        if (title_state != nullptr && vt_state.osc_capture_payload)
                        {
                            switch (vt_state.osc_action)
                            {
                            case 0U:
                            case 1U:
                            case 2U:
                            case 21U:
                                (void)title_state->set_title(std::wstring_view(vt_state.osc_payload.data(), vt_state.osc_payload_length));
                                break;
                            default:
                                break;
                            }
                        }

                        vt_state.phase = Phase::ground;
                        vt_state.osc_param = 0;
                        vt_state.osc_param_have_digits = false;
                        vt_state.osc_in_param = true;
                        vt_state.osc_action = 0;
                        vt_state.osc_capture_payload = false;
                        vt_state.osc_payload_length = 0;
                        ++offset;
                        continue;
                    }

                    if (ch == L'\x1b')
                    {
                        vt_state.phase = Phase::osc_escape;
                        ++offset;
                        continue;
                    }

                    if (vt_state.osc_capture_payload && vt_state.osc_payload_length < vt_state.osc_payload.size())
                    {
                        vt_state.osc_payload[vt_state.osc_payload_length++] = ch;
                    }

                    ++offset;
                    continue;
                }
                case Phase::osc_escape:
                {
                    if (ch == L'\\')
                    {
                        if (title_state != nullptr && vt_state.osc_capture_payload)
                        {
                            switch (vt_state.osc_action)
                            {
                            case 0U:
                            case 1U:
                            case 2U:
                            case 21U:
                                (void)title_state->set_title(std::wstring_view(vt_state.osc_payload.data(), vt_state.osc_payload_length));
                                break;
                            default:
                                break;
                            }
                        }

                        vt_state.phase = Phase::ground;
                        vt_state.osc_param = 0;
                        vt_state.osc_param_have_digits = false;
                        vt_state.osc_in_param = true;
                        vt_state.osc_action = 0;
                        vt_state.osc_capture_payload = false;
                        vt_state.osc_payload_length = 0;
                        ++offset;
                        continue;
                    }

                    vt_state.phase = Phase::osc;
                    continue;
                }
                case Phase::string:
                {
                    if (ch == L'\x9c')
                    {
                        vt_state.phase = Phase::ground;
                        ++offset;
                        continue;
                    }

                    if (ch == L'\x1b')
                    {
                        vt_state.phase = Phase::string_escape;
                        ++offset;
                        continue;
                    }

                    ++offset;
                    continue;
                }
                case Phase::string_escape:
                {
                    if (ch == L'\\')
                    {
                        vt_state.phase = Phase::ground;
                        ++offset;
                        continue;
                    }

                    vt_state.phase = Phase::string;
                    continue;
                }
                case Phase::ground:
                    break;
                }

                if (ch == L'\x1b')
                {
                    vt_state.phase = Phase::escape;
                    ++offset;
                    continue;
                }

                if (ch == L'\x009b')
                {
                    vt_state.phase = Phase::csi;
                    vt_state.csi = {};
                    vt_state.csi_current = 0;
                    vt_state.csi_have_digits = false;
                    vt_state.csi_last_was_separator = false;
                    vt_state.csi_length = 0;
                    ++offset;
                    continue;
                }

                if (ch == L'\x9d')
                {
                    vt_state.phase = Phase::osc;
                    vt_state.osc_param = 0;
                    vt_state.osc_param_have_digits = false;
                    vt_state.osc_in_param = true;
                    vt_state.osc_action = 0;
                    vt_state.osc_capture_payload = false;
                    vt_state.osc_payload_length = 0;
                    ++offset;
                    continue;
                }

                if (ch == L'\x9c')
                {
                    ++offset;
                    continue;
                }

                if (ch == L'\x90' || ch == L'\x98' || ch == L'\x9e' || ch == L'\x9f')
                {
                    vt_state.phase = Phase::string;
                    ++offset;
                    continue;
                }
            }
            if (processed_output)
            {
                switch (ch)
                {
                case L'\r':
                    cursor.X = 0;
                    ++offset;
                    continue;
                case L'\n':
                    if (!disable_newline_auto_return)
                    {
                        cursor.X = 0;
                    }
                    line_feed();
                    ++offset;
                    continue;
                case L'\b':
                    if (cursor.X > 0)
                    {
                        --cursor.X;
                    }
                    ++offset;
                    continue;
                case L'\t':
                {
                    constexpr int tab_width = 8;
                    const int tab_offset = cursor.X < 0 ? 0 : static_cast<int>(cursor.X) % tab_width;
                    const int spaces = tab_width - tab_offset;
                    for (int i = 0; i < spaces; ++i)
                    {
                        write_printable(L' ');
                    }
                    ++offset;
                    continue;
                }
                default:
                    break;
                }
            }

            write_printable(ch);
            ++offset;
        }

        if (vt_autowrap != original_vt_autowrap)
        {
            screen_buffer.set_vt_autowrap_enabled(vt_autowrap);
        }

        if (vt_origin_mode != original_vt_origin_mode)
        {
            screen_buffer.set_vt_origin_mode_enabled(vt_origin_mode);
        }

        if (vt_insert_mode != original_vt_insert_mode)
        {
            screen_buffer.set_vt_insert_mode_enabled(vt_insert_mode);
        }

        screen_buffer.set_vt_delayed_wrap_position(vt_delayed_wrap_position);
        screen_buffer.set_cursor_position(cursor);
        screen_buffer.set_text_attributes(attributes);
        screen_buffer.snap_window_to_cursor();
    }

    [[nodiscard]] inline std::expected<size_t, DeviceCommError> wide_to_multibyte_length(
        const std::wstring_view value,
        const UINT code_page,
        const wchar_t* const context) noexcept
    {
        if (context == nullptr)
        {
            return std::unexpected(DeviceCommError{ .context = L"wide_to_multibyte_length context was null", .win32_error = ERROR_INVALID_PARAMETER });
        }

        if (value.empty())
        {
            return size_t{ 0 };
        }

        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ERROR_INVALID_DATA });
        }

        const int required = ::WideCharToMultiByte(
            code_page,
            0,
            value.data(),
            static_cast<int>(value.size()),
            nullptr,
            0,
            nullptr,
            nullptr);
        if (required <= 0)
        {
            return std::unexpected(DeviceCommError{ .context = context, .win32_error = ::GetLastError() });
        }

        return static_cast<size_t>(required);
    }

    template<typename Comm, typename HostIo = NullHostIo>
    [[nodiscard]] std::expected<DispatchOutcome, DeviceCommError> dispatch_message(
        ServerState& state,
        BasicApiMessage<Comm>& message,
        HostIo& host_io) noexcept
    {
        DispatchOutcome outcome{};

        const auto& descriptor = message.descriptor();
        switch (descriptor.function)
        {
        case console_io_user_defined:
        {
            auto& packet = message.packet();
            const auto api_number = packet.payload.user_defined.msg_header.ApiNumber;
            const auto api_size = packet.payload.user_defined.msg_header.ApiDescriptorSize;

            if (api_size > sizeof(packet.payload.user_defined.u))
            {
                message.set_reply_status(core::status_invalid_parameter);
                message.set_reply_information(0);
                return outcome;
            }

            // Mirror the upstream response behavior: always return the API descriptor bytes.
            message.completion().write.data = &packet.payload.user_defined.u;
            message.completion().write.size = api_size;
            message.completion().write.offset = 0;

            message.set_write_offset(api_size);
            message.set_read_offset(api_size + sizeof(CONSOLE_MSG_HEADER));

            const auto reject_user_defined_not_implemented = [&]() noexcept {
                // Even when the operation is rejected, conhost returns the API descriptor bytes.
                // Zero-fill them to keep replies deterministic and avoid leaking meaningless client
                // input for deprecated/unsupported operations.
                std::memset(&packet.payload.user_defined.u, 0, api_size);
                message.set_reply_status(core::status_not_implemented);
                message.set_reply_information(0);
            };

            // Minimal subset: layer 1 mode and code page APIs.
            if (api_number == static_cast<ULONG>(ConsolepGetMode))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (handle->kind == ObjectKind::input)
                {
                    packet.payload.user_defined.u.console_msg_l1.GetConsoleMode.Mode = state.input_mode();
                }
                else
                {
                    packet.payload.user_defined.u.console_msg_l1.GetConsoleMode.Mode = state.output_mode();
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetMode))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                const ULONG requested = packet.payload.user_defined.u.console_msg_l1.SetConsoleMode.Mode;
                if (handle->kind == ObjectKind::input)
                {
                    state.set_input_mode(requested);

                    // Conhost compatibility: input modes are applied even if the call ultimately
                    // returns an error for an invalid combination/unknown bits.
                    constexpr ULONG input_modes =
                        ENABLE_LINE_INPUT |
                        ENABLE_PROCESSED_INPUT |
                        ENABLE_ECHO_INPUT |
                        ENABLE_WINDOW_INPUT |
                        ENABLE_MOUSE_INPUT |
                        ENABLE_VIRTUAL_TERMINAL_INPUT;
                    constexpr ULONG private_modes =
                        ENABLE_INSERT_MODE |
                        ENABLE_QUICK_EDIT_MODE |
                        ENABLE_AUTO_POSITION |
                        ENABLE_EXTENDED_FLAGS;
                    constexpr ULONG valid_bits = input_modes | private_modes;

                    const bool has_invalid_bits = (requested & ~valid_bits) != 0;
                    const bool echo_without_line = (requested & ENABLE_ECHO_INPUT) != 0 &&
                                                   (requested & ENABLE_LINE_INPUT) == 0;
                    const auto status = (has_invalid_bits || echo_without_line)
                        ? core::status_invalid_parameter
                        : core::status_success;
                    message.set_reply_status(status);
                }
                else
                {
                    constexpr ULONG valid_bits =
                        ENABLE_PROCESSED_OUTPUT |
                        ENABLE_WRAP_AT_EOL_OUTPUT |
                        ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                        DISABLE_NEWLINE_AUTO_RETURN |
                        ENABLE_LVB_GRID_WORLDWIDE;

                    if ((requested & ~valid_bits) != 0)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    state.set_output_mode(requested);
                    message.set_reply_status(core::status_success);
                }

                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetCP))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleCP;
                body.CodePage = body.Output ? state.output_code_page() : state.input_code_page();

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetCP))
            {
                const auto& body = packet.payload.user_defined.u.console_msg_l2.SetConsoleCP;
                if (body.Output)
                {
                    state.set_output_code_page(body.CodePage);
                }
                else
                {
                    state.set_input_code_page(body.CodePage);
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetCursorInfo))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.GetConsoleCursorInfo;
                body.CursorSize = screen_buffer->cursor_size();
                body.Visible = screen_buffer->cursor_visible() ? TRUE : FALSE;

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetCursorInfo))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto& body = packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorInfo;
                if (body.CursorSize < 1 || body.CursorSize > 100)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                screen_buffer->set_cursor_info(body.CursorSize, body.Visible != FALSE);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetScreenBufferInfo))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.GetConsoleScreenBufferInfo;
                body.Size = screen_buffer->screen_buffer_size();
                body.CursorPosition = screen_buffer->cursor_position();
                const auto window_rect = screen_buffer->window_rect();
                body.ScrollPosition = screen_buffer->scroll_position();
                body.Attributes = screen_buffer->text_attributes();
                // ConDrv's `CurrentWindowSize` is expressed as an inclusive delta (Right-Left, Bottom-Top),
                // matching how the inbox conhost populates `CONSOLE_SCREENBUFFERINFO_MSG`.
                body.CurrentWindowSize.X = static_cast<SHORT>(window_rect.Right - window_rect.Left);
                body.CurrentWindowSize.Y = static_cast<SHORT>(window_rect.Bottom - window_rect.Top);
                body.MaximumWindowSize = screen_buffer->maximum_window_size();
                body.PopupAttributes = screen_buffer->text_attributes();
                body.FullscreenSupported = FALSE;

                const auto& table = screen_buffer->color_table();
                for (size_t i = 0; i < table.size(); ++i)
                {
                    body.ColorTable[i] = table[i];
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetScreenBufferInfo))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.SetConsoleScreenBufferInfo;
                if (body.Size.X <= 0 || body.Size.Y <= 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (!screen_buffer->set_screen_buffer_size(body.Size))
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (body.CursorPosition.X < 0 ||
                    body.CursorPosition.Y < 0 ||
                    body.CursorPosition.X >= body.Size.X ||
                    body.CursorPosition.Y >= body.Size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                screen_buffer->set_cursor_position(body.CursorPosition);
                screen_buffer->set_text_attributes(body.Attributes);
                screen_buffer->set_default_text_attributes(body.Attributes);
                screen_buffer->set_color_table(body.ColorTable);

                if (body.ScrollPosition.X < 0 || body.ScrollPosition.Y < 0 ||
                    body.CurrentWindowSize.X < 0 || body.CurrentWindowSize.Y < 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                {
                    const long left = body.ScrollPosition.X;
                    const long top = body.ScrollPosition.Y;
                    const long right = left + body.CurrentWindowSize.X;
                    const long bottom = top + body.CurrentWindowSize.Y;
                    if (right < left || bottom < top)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    const auto size = screen_buffer->screen_buffer_size();
                    if (left >= size.X || top >= size.Y || right >= size.X || bottom >= size.Y)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    SMALL_RECT window{};
                    window.Left = static_cast<SHORT>(left);
                    window.Top = static_cast<SHORT>(top);
                    window.Right = static_cast<SHORT>(right);
                    window.Bottom = static_cast<SHORT>(bottom);
                    if (!screen_buffer->set_window_rect(window))
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetScreenBufferSize))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto requested = packet.payload.user_defined.u.console_msg_l2.SetConsoleScreenBufferSize.Size;
                if (requested.X <= 0 || requested.Y <= 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (!screen_buffer->set_screen_buffer_size(requested))
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetCursorPosition))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto position = packet.payload.user_defined.u.console_msg_l2.SetConsoleCursorPosition.CursorPosition;
                const auto size = screen_buffer->screen_buffer_size();
                if (position.X < 0 || position.Y < 0 || position.X >= size.X || position.Y >= size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                screen_buffer->set_cursor_position(position);
                screen_buffer->snap_window_to_cursor();
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetLargestWindowSize))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                packet.payload.user_defined.u.console_msg_l2.GetLargestConsoleWindowSize.Size = screen_buffer->maximum_window_size();
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepScrollScreenBuffer))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.ScrollConsoleScreenBuffer;
                const auto size = screen_buffer->screen_buffer_size();
                if (size.X <= 0 || size.Y <= 0)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto scroll = body.ScrollRectangle;
                if (scroll.Left > scroll.Right || scroll.Top > scroll.Bottom)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const SHORT max_x = static_cast<SHORT>(size.X - 1);
                const SHORT max_y = static_cast<SHORT>(size.Y - 1);

                scroll.Left = scroll.Left < 0 ? 0 : scroll.Left;
                scroll.Top = scroll.Top < 0 ? 0 : scroll.Top;
                scroll.Right = scroll.Right > max_x ? max_x : scroll.Right;
                scroll.Bottom = scroll.Bottom > max_y ? max_y : scroll.Bottom;

                if (scroll.Left > scroll.Right || scroll.Top > scroll.Bottom)
                {
                    message.set_reply_status(core::status_success);
                    message.set_reply_information(0);
                    return outcome;
                }

                SMALL_RECT clip{ 0, 0, max_x, max_y };
                if (body.Clip != FALSE)
                {
                    clip = body.ClipRectangle;
                    if (clip.Left > clip.Right || clip.Top > clip.Bottom)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    clip.Left = clip.Left < 0 ? 0 : clip.Left;
                    clip.Top = clip.Top < 0 ? 0 : clip.Top;
                    clip.Right = clip.Right > max_x ? max_x : clip.Right;
                    clip.Bottom = clip.Bottom > max_y ? max_y : clip.Bottom;

                    if (clip.Left > clip.Right || clip.Top > clip.Bottom)
                    {
                        message.set_reply_status(core::status_success);
                        message.set_reply_information(0);
                        return outcome;
                    }
                }

                const wchar_t fill_char = body.Unicode != FALSE
                    ? body.Fill.Char.UnicodeChar
                    : static_cast<wchar_t>(static_cast<unsigned char>(body.Fill.Char.AsciiChar));

                if (!screen_buffer->scroll_screen_buffer(scroll, clip, body.DestinationOrigin, fill_char, body.Fill.Attributes))
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetTextAttribute))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                screen_buffer->set_text_attributes(packet.payload.user_defined.u.console_msg_l2.SetConsoleTextAttribute.Attributes);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetWindowInfo))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto& body = packet.payload.user_defined.u.console_msg_l2.SetConsoleWindowInfo;
                SMALL_RECT desired_window{};
                if (body.Absolute == FALSE)
                {
                    // Relative mode: apply deltas to the current viewport edges.
                    const auto current = screen_buffer->window_rect();
                    const auto& delta = body.Window;

                    const long left = static_cast<long>(current.Left) + static_cast<long>(delta.Left);
                    const long top = static_cast<long>(current.Top) + static_cast<long>(delta.Top);
                    const long right = static_cast<long>(current.Right) + static_cast<long>(delta.Right);
                    const long bottom = static_cast<long>(current.Bottom) + static_cast<long>(delta.Bottom);

                    if (right < left || bottom < top)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    const auto buffer_size = screen_buffer->screen_buffer_size();
                    if (left < 0 || top < 0 || right < 0 || bottom < 0 ||
                        left >= buffer_size.X || top >= buffer_size.Y ||
                        right >= buffer_size.X || bottom >= buffer_size.Y)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    desired_window.Left = static_cast<SHORT>(left);
                    desired_window.Top = static_cast<SHORT>(top);
                    desired_window.Right = static_cast<SHORT>(right);
                    desired_window.Bottom = static_cast<SHORT>(bottom);
                }
                else
                {
                    desired_window = body.Window;
                }

                if (!screen_buffer->set_window_rect(desired_window))
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGenerateCtrlEvent))
            {
                const auto& body = packet.payload.user_defined.u.console_msg_l2.GenerateConsoleCtrlEvent;

                DWORD ctrl_flags = 0;
                switch (body.CtrlEvent)
                {
                case CTRL_C_EVENT:
                    ctrl_flags = static_cast<DWORD>(core::console_ctrl_c_flag);
                    break;
                case CTRL_BREAK_EVENT:
                    ctrl_flags = static_cast<DWORD>(core::console_ctrl_break_flag);
                    break;
                default:
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                state.for_each_process([&](const ProcessState& process) noexcept {
                    if (body.ProcessGroupId != 0 && process.pid != body.ProcessGroupId)
                    {
                        return;
                    }

                    (void)host_io.send_end_task(process.pid, body.CtrlEvent, ctrl_flags);
                });

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetActiveScreenBuffer))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (!state.set_active_screen_buffer(handle->screen_buffer))
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepFlushInputBuffer))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::input)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (auto flushed = host_io.flush_input_buffer(); !flushed)
                {
                    return std::unexpected(flushed.error());
                }

                // Flushing input drops any pending decoded units that were held back due to a small
                // output buffer (e.g. the second code unit of a surrogate pair).
                handle->decoded_input_pending.reset();
                handle->pending_input_bytes.clear();
                handle->cooked_read_pending.clear();
                handle->cooked_line_in_progress.clear();
                handle->cooked_line_cursor = 0;
                handle->cooked_insert_mode = true;

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepWriteConsoleInput))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::input)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.WriteConsoleInput;
                body.NumRecords = 0;

                if ((input->size() % sizeof(INPUT_RECORD)) != 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const size_t record_count = input->size() / sizeof(INPUT_RECORD);
                const auto* records = reinterpret_cast<const INPUT_RECORD*>(input->data());

                if (body.Append == FALSE)
                {
                    if (auto flushed = host_io.flush_input_buffer(); !flushed)
                    {
                        return std::unexpected(flushed.error());
                    }

                    // Replacing the input queue must also reset per-handle decode/cooked state so
                    // subsequent reads do not observe stale partial-sequence or cooked-line state.
                    handle->decoded_input_pending.reset();
                    handle->pending_input_bytes.clear();
                    handle->cooked_read_pending.clear();
                    handle->cooked_line_in_progress.clear();
                    handle->cooked_line_cursor = 0;
                    handle->cooked_insert_mode = true;
                }

                std::vector<std::byte> bytes;
                try
                {
                    // Worst case: UTF-8 may take up to 4 bytes per UTF-16 code unit.
                    constexpr size_t max_bytes_per_unit = 4;
                    const size_t reserve_hint = record_count > (std::numeric_limits<size_t>::max() / max_bytes_per_unit)
                        ? std::numeric_limits<size_t>::max()
                        : record_count * max_bytes_per_unit;
                    bytes.reserve(reserve_hint);
                }
                catch (...)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                const UINT code_page = state.input_code_page() == 0 ? ::GetOEMCP() : static_cast<UINT>(state.input_code_page());
                try
                {
                    for (size_t i = 0; i < record_count; ++i)
                    {
                        const auto& record = records[i];
                        if (record.EventType != KEY_EVENT)
                        {
                            continue;
                        }

                        const auto& key = record.Event.KeyEvent;
                        if (key.bKeyDown == FALSE)
                        {
                            continue;
                        }

                        std::array<char, 8> encoded{};
                        int encoded_bytes = 0;
                        if (body.Unicode != FALSE)
                        {
                            const wchar_t ch = key.uChar.UnicodeChar;
                            if (ch == L'\0')
                            {
                                continue;
                            }

                            encoded_bytes = ::WideCharToMultiByte(
                                code_page,
                                0,
                                &ch,
                                1,
                                encoded.data(),
                                static_cast<int>(encoded.size()),
                                nullptr,
                                nullptr);
                            if (encoded_bytes <= 0)
                            {
                                encoded[0] = '?';
                                encoded_bytes = 1;
                            }
                        }
                        else
                        {
                            const char ch = key.uChar.AsciiChar;
                            if (ch == '\0')
                            {
                                continue;
                            }

                            encoded[0] = ch;
                            encoded_bytes = 1;
                        }

                        const WORD repeat = key.wRepeatCount == 0 ? 1 : key.wRepeatCount;
                        for (WORD r = 0; r < repeat; ++r)
                        {
                            bytes.insert(
                                bytes.end(),
                                reinterpret_cast<const std::byte*>(encoded.data()),
                                reinterpret_cast<const std::byte*>(encoded.data()) + static_cast<size_t>(encoded_bytes));
                        }
                    }
                }
                catch (...)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (!host_io.inject_input_bytes(std::span<const std::byte>(bytes.data(), bytes.size())))
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.NumRecords = record_count > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(record_count);

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepFillConsoleOutput))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.FillConsoleOutput;
                const ULONG requested = body.Length;
                body.Length = 0;

                const auto size = screen_buffer->screen_buffer_size();
                if (size.X <= 0 || size.Y <= 0)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto origin = body.WriteCoord;
                if (origin.X < 0 || origin.Y < 0 || origin.X >= size.X || origin.Y >= size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                size_t written = 0;
                switch (body.ElementType)
                {
                case CONSOLE_ATTRIBUTE:
                    written = screen_buffer->fill_output_attributes(origin, body.Element, requested);
                    break;
                case CONSOLE_REAL_UNICODE:
                case CONSOLE_FALSE_UNICODE:
                    written = screen_buffer->fill_output_characters(origin, static_cast<wchar_t>(body.Element), requested);
                    break;
                case CONSOLE_ASCII:
                    written = screen_buffer->fill_output_characters(
                        origin,
                        static_cast<wchar_t>(static_cast<unsigned char>(body.Element)),
                        requested);
                    break;
                default:
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.Length = static_cast<ULONG>(written);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepReadConsoleOutputString))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutputString;
                body.NumRecords = 0;

                const auto size = screen_buffer->screen_buffer_size();
                if (size.X <= 0 || size.Y <= 0)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto origin = body.ReadCoord;
                if (origin.X < 0 || origin.Y < 0 || origin.X >= size.X || origin.Y >= size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                size_t records_read = 0;
                switch (body.StringType)
                {
                case CONSOLE_ATTRIBUTE:
                {
                    const size_t max_records = output->size() / sizeof(USHORT);
                    auto* words = reinterpret_cast<USHORT*>(output->data());
                    records_read = screen_buffer->read_output_attributes(origin, std::span<USHORT>(words, max_records));
                    break;
                }
                case CONSOLE_REAL_UNICODE:
                case CONSOLE_FALSE_UNICODE:
                {
                    const size_t max_records = output->size() / sizeof(wchar_t);
                    auto* chars = reinterpret_cast<wchar_t*>(output->data());
                    records_read = screen_buffer->read_output_characters(origin, std::span<wchar_t>(chars, max_records));
                    break;
                }
                case CONSOLE_ASCII:
                    records_read = screen_buffer->read_output_ascii(origin, *output);
                    break;
                default:
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.NumRecords = static_cast<ULONG>(records_read);
                message.set_reply_status(core::status_success);
                message.set_reply_information(static_cast<ULONG_PTR>(output->size()));
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepWriteConsoleOutput))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.WriteConsoleOutput;
                auto region = body.CharRegion;
                if (region.Left > region.Right || region.Top > region.Bottom)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto size = screen_buffer->screen_buffer_size();
                if (size.X <= 0 || size.Y <= 0)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (region.Left < 0 ||
                    region.Top < 0 ||
                    region.Right >= size.X ||
                    region.Bottom >= size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                if ((input->size() % sizeof(CHAR_INFO)) != 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const size_t width = static_cast<size_t>(region.Right - region.Left + 1);
                const size_t height = static_cast<size_t>(region.Bottom - region.Top + 1);
                const size_t record_count = width * height;

                const size_t available = input->size() / sizeof(CHAR_INFO);
                if (available < record_count)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto* records = reinterpret_cast<const CHAR_INFO*>(input->data());
                const size_t written = screen_buffer->write_output_char_info_rect(region, std::span<const CHAR_INFO>(records, record_count), body.Unicode != FALSE);
                if (written != record_count)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepWriteConsoleOutputString))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.WriteConsoleOutputString;
                body.NumRecords = 0;

                const auto size = screen_buffer->screen_buffer_size();
                if (size.X <= 0 || size.Y <= 0)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto origin = body.WriteCoord;
                if (origin.X < 0 || origin.Y < 0 || origin.X >= size.X || origin.Y >= size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                size_t used = 0;
                switch (body.StringType)
                {
                case CONSOLE_ATTRIBUTE:
                {
                    if ((input->size() % sizeof(USHORT)) != 0)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    const size_t record_count = input->size() / sizeof(USHORT);
                    std::vector<USHORT> attributes;
                    try
                    {
                        attributes.resize(record_count);
                        std::memcpy(attributes.data(), input->data(), input->size());
                    }
                    catch (...)
                    {
                        message.set_reply_status(core::status_no_memory);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    used = screen_buffer->write_output_attributes(origin, attributes);
                    break;
                }
                case CONSOLE_REAL_UNICODE:
                case CONSOLE_FALSE_UNICODE:
                {
                    if ((input->size() % sizeof(wchar_t)) != 0)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    const size_t wchar_count = input->size() / sizeof(wchar_t);
                    std::wstring text;
                    try
                    {
                        text.resize(wchar_count);
                        std::memcpy(text.data(), input->data(), input->size());
                    }
                    catch (...)
                    {
                        message.set_reply_status(core::status_no_memory);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    used = screen_buffer->write_output_characters(origin, text);
                    break;
                }
                case CONSOLE_ASCII:
                    used = screen_buffer->write_output_ascii(origin, *input);
                    break;
                default:
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.NumRecords = static_cast<ULONG>(used);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepReadConsoleOutput))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.ReadConsoleOutput;
                auto region = body.CharRegion;
                if (region.Left > region.Right || region.Top > region.Bottom)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto size = screen_buffer->screen_buffer_size();
                if (size.X <= 0 || size.Y <= 0)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (region.Left < 0 ||
                    region.Top < 0 ||
                    region.Right >= size.X ||
                    region.Bottom >= size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                if ((output->size() % sizeof(CHAR_INFO)) != 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const size_t width = static_cast<size_t>(region.Right - region.Left + 1);
                const size_t height = static_cast<size_t>(region.Bottom - region.Top + 1);
                const size_t record_count = width * height;
                const size_t capacity = output->size() / sizeof(CHAR_INFO);

                if (capacity < record_count)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* records = reinterpret_cast<CHAR_INFO*>(output->data());
                const size_t copied = screen_buffer->read_output_char_info_rect(region, std::span<CHAR_INFO>(records, record_count), body.Unicode != FALSE);
                if (copied != record_count)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(static_cast<ULONG_PTR>(copied * sizeof(CHAR_INFO)));
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetTitle))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l2.GetConsoleTitle;
                const std::wstring_view stored_title = state.title(body.Original != FALSE);

                const size_t needed = stored_title.size();
                body.TitleLength = needed > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(needed);

                if (body.Unicode)
                {
                    const size_t capacity = output->size() / sizeof(wchar_t);
                    const size_t copy_count = capacity == 0 ? 0 : (stored_title.size() >= capacity ? capacity - 1 : stored_title.size());
                    const size_t written = capacity == 0 ? 0 : std::min(capacity, stored_title.size());

                    auto* out_chars = reinterpret_cast<wchar_t*>(output->data());
                    if (capacity != 0)
                    {
                        out_chars[0] = L'\0';
                    }
                    if (copy_count != 0)
                    {
                        std::memcpy(out_chars, stored_title.data(), copy_count * sizeof(wchar_t));
                    }
                    if (capacity != 0)
                    {
                        out_chars[copy_count < capacity ? copy_count : (capacity - 1)] = L'\0';
                    }

                    message.set_reply_status(core::status_success);
                    message.set_reply_information(static_cast<ULONG_PTR>(written * sizeof(wchar_t)));
                    return outcome;
                }

                // A variant: legacy behavior is "all or nothing" when the buffer
                // can't hold the non-null-terminated string.
                const UINT cp = state.output_code_page() == 0 ? ::GetOEMCP() : static_cast<UINT>(state.output_code_page());
                const int required = stored_title.empty()
                    ? 0
                    : ::WideCharToMultiByte(
                        cp,
                        0,
                        stored_title.data(),
                        static_cast<int>(stored_title.size()),
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                if (required < 0)
                {
                    return std::unexpected(DeviceCommError{
                        .context = L"WideCharToMultiByte size query failed for console title",
                        .win32_error = ::GetLastError(),
                    });
                }

                const size_t required_bytes = static_cast<size_t>(required);
                if (!output->empty())
                {
                    output->front() = std::byte{ 0 };
                }

                if (required_bytes == 0)
                {
                    message.set_reply_status(core::status_success);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (output->size() < required_bytes)
                {
                    if (!output->empty())
                    {
                        message.set_reply_information(1);
                    }
                    else
                    {
                        message.set_reply_information(0);
                    }
                    body.TitleLength = 0;
                    message.set_reply_status(core::status_success);
                    return outcome;
                }

                std::vector<char> converted;
                try
                {
                    converted.resize(required_bytes);
                }
                catch (...)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                const int converted_bytes = ::WideCharToMultiByte(
                    cp,
                    0,
                    stored_title.data(),
                    static_cast<int>(stored_title.size()),
                    converted.data(),
                    static_cast<int>(converted.size()),
                    nullptr,
                    nullptr);
                if (converted_bytes <= 0 || static_cast<size_t>(converted_bytes) != required_bytes)
                {
                    return std::unexpected(DeviceCommError{
                        .context = L"WideCharToMultiByte failed for console title",
                        .win32_error = ::GetLastError(),
                    });
                }

                std::memcpy(output->data(), converted.data(), required_bytes);
                const size_t written = output->size() > required_bytes ? required_bytes + 1 : required_bytes;
                if (output->size() > required_bytes)
                {
                    output->data()[required_bytes] = std::byte{ 0 };
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(static_cast<ULONG_PTR>(written));
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetTitle))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                const auto& body = packet.payload.user_defined.u.console_msg_l2.SetConsoleTitle;
                std::wstring title;
                if (body.Unicode)
                {
                    if ((input->size() % sizeof(wchar_t)) != 0)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    const size_t wchar_count = input->size() / sizeof(wchar_t);
                    try
                    {
                        title.resize(wchar_count);
                        std::memcpy(title.data(), input->data(), input->size());
                    }
                    catch (...)
                    {
                        message.set_reply_status(core::status_no_memory);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    if (!title.empty() && title.back() == L'\0')
                    {
                        while (!title.empty() && title.back() == L'\0')
                        {
                            title.pop_back();
                        }
                    }
                }
                else
                {
                    const UINT cp = state.output_code_page() == 0 ? ::GetOEMCP() : static_cast<UINT>(state.output_code_page());
                    const int required = input->empty()
                        ? 0
                        : ::MultiByteToWideChar(
                            cp,
                            0,
                            reinterpret_cast<const char*>(input->data()),
                            static_cast<int>(input->size()),
                            nullptr,
                            0);
                    if (required < 0)
                    {
                        return std::unexpected(DeviceCommError{
                            .context = L"MultiByteToWideChar size query failed for console title",
                            .win32_error = ::GetLastError(),
                        });
                    }

                    try
                    {
                        title.resize(static_cast<size_t>(required));
                    }
                    catch (...)
                    {
                        message.set_reply_status(core::status_no_memory);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    if (required != 0)
                    {
                        const int converted = ::MultiByteToWideChar(
                            cp,
                            0,
                            reinterpret_cast<const char*>(input->data()),
                            static_cast<int>(input->size()),
                            title.data(),
                            required);
                        if (converted != required)
                        {
                            return std::unexpected(DeviceCommError{
                                .context = L"MultiByteToWideChar failed for console title",
                                .win32_error = ::GetLastError(),
                            });
                        }
                    }
                }

                if (!state.set_title(std::move(title)))
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

                if (api_number == static_cast<ULONG>(ConsolepGetNumberOfInputEvents))
                {
                    const auto handle_id = descriptor.object;
                    auto* handle = state.find_object(handle_id);
                    if (handle == nullptr || handle->kind != ObjectKind::input)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                const size_t ready_bytes = host_io.input_bytes_available();
                size_t ready_events = ready_bytes;
                if (ready_bytes != 0 || !handle->pending_input_bytes.empty())
                {
                    // ConDrv reports input events, not raw bytes. The replacement is byte-stream-backed,
                    // so we approximate the visible event count by scanning a bounded prefix using the
                    // VT/code-page token decoder.
                    constexpr size_t peek_limit = 64 * 1024;

                    const size_t pending_byte_count = handle->pending_input_bytes.size();
                    const size_t queue_budget = peek_limit > pending_byte_count ? peek_limit - pending_byte_count : 0;
                    const size_t to_peek = std::min(ready_bytes, queue_budget);

                    std::vector<std::byte> peeked;
                    try
                    {
                        peeked.resize(pending_byte_count + to_peek);
                    }
                    catch (...)
                    {
                        peeked.clear();
                    }

                    if (!peeked.empty())
                    {
                        if (pending_byte_count != 0)
                        {
                            const auto prefix = handle->pending_input_bytes.bytes();
                            std::memcpy(peeked.data(), prefix.data(), pending_byte_count);
                        }

                        size_t peeked_count = pending_byte_count;
                        if (to_peek != 0)
                        {
                            auto read = host_io.peek_input_bytes(std::span<std::byte>(peeked.data() + pending_byte_count, to_peek));
                            if (!read)
                            {
                                return std::unexpected(read.error());
                            }
                            peeked_count += read.value();
                        }

                        ready_events = 0;
                        const UINT code_page = static_cast<UINT>(state.input_code_page());
                        const bool processed_input = (state.input_mode() & ENABLE_PROCESSED_INPUT) != 0;

                        size_t offset = 0;
                        const auto bytes = std::span<const std::byte>(peeked.data(), peeked_count);
                        while (offset < bytes.size())
                        {
                            vt_input::DecodedToken token{};
                            const auto decode_outcome = decode_one_input_token(code_page, bytes.subspan(offset), token);
                            if (decode_outcome == InputDecodeOutcome::need_more_data)
                            {
                                break;
                            }

                            if (token.bytes_consumed == 0)
                            {
                                break;
                            }

                            offset += token.bytes_consumed;

                            if (token.kind == vt_input::TokenKind::ignored_sequence)
                            {
                                continue;
                            }

                            if (token.kind == vt_input::TokenKind::key_event)
                            {
                                if (processed_input &&
                                    token.key.bKeyDown &&
                                    (key_event_matches_ctrl_c(token.key) || key_event_matches_ctrl_break(token.key)))
                                {
                                    continue;
                                }

                                if (ready_events != std::numeric_limits<size_t>::max())
                                {
                                    ++ready_events;
                                }
                                continue;
                            }

                            const auto& text = token.text;
                            if (text.char_count == 0)
                            {
                                continue;
                            }

                            if (processed_input && text.char_count == 1 && text.chars[0] == static_cast<wchar_t>(0x0003))
                            {
                                continue;
                            }

                            if (ready_events == std::numeric_limits<size_t>::max())
                            {
                                continue;
                            }

                            const size_t remaining = std::numeric_limits<size_t>::max() - ready_events;
                            ready_events = text.char_count > remaining ? std::numeric_limits<size_t>::max() : ready_events + text.char_count;
                        }
                    }
                }

                if (handle->decoded_input_pending && ready_events != std::numeric_limits<size_t>::max())
                {
                    ++ready_events;
                }

                packet.payload.user_defined.u.console_msg_l1.GetNumberOfConsoleInputEvents.ReadyEvents =
                    ready_events > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(ready_events);

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetLangId))
            {
                packet.payload.user_defined.u.console_msg_l1.GetConsoleLangId.LangId = ::GetUserDefaultLangID();
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepNotifyLastClose))
            {
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepMapBitmap))
            {
                reject_user_defined_not_implemented();
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetNumberOfFonts))
            {
                packet.payload.user_defined.u.console_msg_l3.GetNumberOfConsoleFonts.NumberOfFonts = 1;
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetFontInfo))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleFontInfo;
                body.NumFonts = 1;

                if (output->size() < sizeof(CONSOLE_FONT_INFO))
                {
                    message.set_reply_status(core::status_buffer_too_small);
                    message.set_reply_information(0);
                    return outcome;
                }

                CONSOLE_FONT_INFO info{};
                info.nFont = state.font_index();
                info.dwFontSize = state.font_size();
                std::memcpy(output->data(), &info, sizeof(info));

                message.set_reply_status(core::status_success);
                message.set_reply_information(sizeof(info));
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetFontSize))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleFontSize;
                if (body.FontIndex != state.font_index())
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.FontSize = state.font_size();
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetCurrentFont))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetCurrentConsoleFont;
                state.fill_current_font(body);

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetFont))
            {
                // Deprecated in the inbox host, but accept and succeed for compatibility.
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetIcon) ||
                api_number == static_cast<ULONG>(ConsolepInvalidateBitmapRect) ||
                api_number == static_cast<ULONG>(ConsolepVDMOperation) ||
                api_number == static_cast<ULONG>(ConsolepSetCursor) ||
                api_number == static_cast<ULONG>(ConsolepShowCursor) ||
                api_number == static_cast<ULONG>(ConsolepMenuControl) ||
                api_number == static_cast<ULONG>(ConsolepSetPalette) ||
                api_number == static_cast<ULONG>(ConsolepRegisterVDM) ||
                api_number == static_cast<ULONG>(ConsolepGetHardwareState) ||
                api_number == static_cast<ULONG>(ConsolepSetHardwareState))
            {
                reject_user_defined_not_implemented();
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetDisplayMode))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleDisplayMode;
                body.ScreenBufferDimensions = screen_buffer->screen_buffer_size();
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetKeyShortcuts))
            {
                const auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleKeyShortcuts;
                state.set_key_shortcuts(body.Set != FALSE, body.ReserveKeys);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetMenuClose))
            {
                const auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleMenuClose;
                state.set_menu_close(body.Enable != FALSE);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepCharType))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleCharType;
                const auto size = screen_buffer->screen_buffer_size();
                if (body.coordCheck.X < 0 || body.coordCheck.Y < 0 ||
                    body.coordCheck.X >= size.X || body.coordCheck.Y >= size.Y)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.dwType = CHAR_TYPE_SBCS;
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetLocalEUDC))
            {
                // Legacy API used by older clients for local EUDC configuration. The
                // inbox host treats this as deprecated; we accept and ignore it.
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetCursorMode))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleCursorMode;
                state.set_cursor_mode(body.Blink != FALSE, body.DBEnable != FALSE);
                body.Blink = state.cursor_blink() ? TRUE : FALSE;
                body.DBEnable = state.cursor_db_enable() ? TRUE : FALSE;
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetCursorMode))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleCursorMode;
                body.Blink = state.cursor_blink() ? TRUE : FALSE;
                body.DBEnable = state.cursor_db_enable() ? TRUE : FALSE;
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepRegisterOS2))
            {
                const auto& body = packet.payload.user_defined.u.console_msg_l3.RegisterConsoleOS2;
                state.set_os2_registered(body.fOs2Register != FALSE);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetOS2OemFormat))
            {
                const auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleOS2OemFormat;
                state.set_os2_oem_format(body.fOs2OemFormat != FALSE);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetNlsMode))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleNlsMode;
                body.Ready = TRUE;
                body.NlsMode = state.nls_mode();
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetNlsMode))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleNlsMode;
                state.set_nls_mode(body.NlsMode);
                body.Ready = TRUE;
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetCurrentFont))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.SetCurrentConsoleFont;
                state.apply_current_font(body);
                state.fill_current_font(body);

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetConsoleWindow))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleWindow;
                body.hwnd = decltype(body.hwnd){};

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetDisplayMode))
            {
                packet.payload.user_defined.u.console_msg_l3.GetConsoleDisplayMode.ModeFlags = 0;
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetKeyboardLayoutName))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetKeyboardLayoutName;

                WCHAR layout[KL_NAMELENGTH]{};
                if (::GetKeyboardLayoutNameW(layout) == FALSE)
                {
                    static constexpr wchar_t fallback[] = L"00000409";
                    static_assert(sizeof(fallback) == sizeof(layout), "Fallback keyboard layout name must match KL_NAMELENGTH");
                    std::memcpy(layout, fallback, sizeof(layout));
                }

                if (body.bAnsi != FALSE)
                {
                    for (size_t i = 0; i < static_cast<size_t>(KL_NAMELENGTH); ++i)
                    {
                        const WCHAR ch = layout[i];
                        body.achLayout[i] = ch <= 0x7F ? static_cast<char>(ch) : '?';
                    }
                }
                else
                {
                    std::memcpy(body.awchLayout, layout, sizeof(body.awchLayout));
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetMouseInfo))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleMouseInfo;
                const int buttons = ::GetSystemMetrics(SM_CMOUSEBUTTONS);
                body.NumButtons = buttons > 0 ? static_cast<ULONG>(buttons) : 0;

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetSelectionInfo))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleSelectionInfo;
                body.SelectionInfo = CONSOLE_SELECTION_INFO{};

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetConsoleProcessList))
            {
                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleProcessList;
                const size_t capacity = output->size() / sizeof(DWORD);

                std::vector<const ProcessState*> processes;
                try
                {
                    processes.reserve(state.process_count());
                }
                catch (...)
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }

                state.for_each_process([&](const ProcessState& process) noexcept { processes.push_back(&process); });
                std::sort(processes.begin(), processes.end(), [](const ProcessState* left, const ProcessState* right) noexcept {
                    if (left == right)
                    {
                        return false;
                    }
                    if (left == nullptr || right == nullptr)
                    {
                        return left != nullptr;
                    }
                    return left->connect_sequence > right->connect_sequence;
                });

                const size_t total = processes.size();
                const ULONG total_clamped =
                    total > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(total);

                if (capacity < total)
                {
                    body.dwProcessCount = total_clamped;
                    message.set_reply_status(core::status_success);
                    message.set_reply_information(0);
                    return outcome;
                }

                for (size_t i = 0; i < total; ++i)
                {
                    const DWORD pid = processes[i] == nullptr ? 0 : processes[i]->pid;
                    const size_t offset = i * sizeof(DWORD);
                    std::memcpy(output->data() + offset, &pid, sizeof(pid));
                }

                body.dwProcessCount = total_clamped;
                message.set_reply_status(core::status_success);
                message.set_reply_information(total * sizeof(DWORD));
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepAddAlias))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                const auto& body = packet.payload.user_defined.u.console_msg_l3.AddConsoleAliasW;
                const bool unicode = body.Unicode != FALSE;
                const size_t exe_length = body.ExeLength;
                const size_t source_length = body.SourceLength;
                const size_t target_length = body.TargetLength;

                const size_t alignment = unicode ? alignof(wchar_t) : size_t{ 1 };
                const bool bad_length = exe_length + source_length + target_length > input->size();
                const bool bad_alignment = ((exe_length | source_length | target_length) & (alignment - 1)) != 0;
                if (bad_length || bad_alignment)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto bytes = std::span<const std::byte>(input->data(), input->size());
                const auto exe_bytes = bytes.subspan(0, exe_length);
                const auto source_bytes = bytes.subspan(exe_length, source_length);
                const auto target_bytes = bytes.subspan(exe_length + source_length, target_length);

                const UINT code_page = static_cast<UINT>(state.output_code_page());

                auto exe_decoded = decode_console_string(unicode, exe_bytes, code_page, L"ConsolepAddAlias exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto source_decoded = decode_console_string(unicode, source_bytes, code_page, L"ConsolepAddAlias source decode failed");
                if (!source_decoded)
                {
                    message.set_reply_status(source_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto target_decoded = decode_console_string(unicode, target_bytes, code_page, L"ConsolepAddAlias target decode failed");
                if (!target_decoded)
                {
                    message.set_reply_status(target_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (source_decoded->empty())
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto exe_norm = fold_to_lower_invariant(*exe_decoded, L"ConsolepAddAlias exe name fold failed");
                if (!exe_norm)
                {
                    message.set_reply_status(exe_norm.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto source_norm = fold_to_lower_invariant(*source_decoded, L"ConsolepAddAlias source fold failed");
                if (!source_norm)
                {
                    message.set_reply_status(source_norm.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto stored = state.set_alias(std::move(exe_norm.value()), std::move(source_norm.value()), std::move(target_decoded.value()));
                if (!stored)
                {
                    message.set_reply_status(stored.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetAlias))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasW;
                body.TargetLength = 0;

                const bool unicode = body.Unicode != FALSE;
                const size_t exe_length = body.ExeLength;
                const size_t source_length = body.SourceLength;

                const size_t alignment = unicode ? alignof(wchar_t) : size_t{ 1 };
                const bool bad_length = exe_length + source_length > input->size();
                const bool bad_alignment = ((exe_length | source_length) & (alignment - 1)) != 0;
                if (bad_length || bad_alignment)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto bytes = std::span<const std::byte>(input->data(), input->size());
                const auto exe_bytes = bytes.subspan(0, exe_length);
                const auto source_bytes = bytes.subspan(exe_length, source_length);

                const UINT code_page = static_cast<UINT>(state.output_code_page());

                auto exe_decoded = decode_console_string(unicode, exe_bytes, code_page, L"ConsolepGetAlias exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto source_decoded = decode_console_string(unicode, source_bytes, code_page, L"ConsolepGetAlias source decode failed");
                if (!source_decoded)
                {
                    message.set_reply_status(source_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto exe_norm = fold_to_lower_invariant(*exe_decoded, L"ConsolepGetAlias exe name fold failed");
                if (!exe_norm)
                {
                    message.set_reply_status(exe_norm.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto source_norm = fold_to_lower_invariant(*source_decoded, L"ConsolepGetAlias source fold failed");
                if (!source_norm)
                {
                    message.set_reply_status(source_norm.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                const auto target = state.try_get_alias(exe_norm.value(), source_norm.value());
                if (!target)
                {
                    message.set_reply_status(core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                const std::wstring_view target_view = target.value();
                if (unicode)
                {
                    const size_t required_bytes = (target_view.size() + 1) * sizeof(wchar_t);
                    if (required_bytes > static_cast<size_t>(std::numeric_limits<USHORT>::max()))
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    body.TargetLength = static_cast<USHORT>(required_bytes);
                    if (output->size() < required_bytes)
                    {
                        message.set_reply_status(core::status_buffer_too_small);
                        message.set_reply_information(body.TargetLength);
                        return outcome;
                    }

                    if (!target_view.empty())
                    {
                        std::memcpy(output->data(), target_view.data(), target_view.size() * sizeof(wchar_t));
                    }

                    const wchar_t terminator = L'\0';
                    std::memcpy(output->data() + (target_view.size() * sizeof(wchar_t)), &terminator, sizeof(terminator));

                    message.set_reply_status(core::status_success);
                    message.set_reply_information(body.TargetLength);
                    return outcome;
                }

                if (target_view.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const int required_narrow = ::WideCharToMultiByte(
                    code_page,
                    0,
                    target_view.data(),
                    static_cast<int>(target_view.size()),
                    nullptr,
                    0,
                    nullptr,
                    nullptr);
                if (required_narrow <= 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const size_t required_bytes = static_cast<size_t>(required_narrow) + 1;
                if (required_bytes > static_cast<size_t>(std::numeric_limits<USHORT>::max()))
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.TargetLength = static_cast<USHORT>(required_bytes);
                if (output->size() < required_bytes)
                {
                    message.set_reply_status(core::status_buffer_too_small);
                    message.set_reply_information(body.TargetLength);
                    return outcome;
                }

                if (required_narrow != 0)
                {
                    const int converted = ::WideCharToMultiByte(
                        code_page,
                        0,
                        target_view.data(),
                        static_cast<int>(target_view.size()),
                        reinterpret_cast<char*>(output->data()),
                        required_narrow,
                        nullptr,
                        nullptr);
                    if (converted != required_narrow)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }
                }

                reinterpret_cast<char*>(output->data())[required_narrow] = '\0';
                message.set_reply_status(core::status_success);
                message.set_reply_information(body.TargetLength);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetAliasesLength))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasesLengthW;
                body.AliasesLength = 0;

                const bool unicode = body.Unicode != FALSE;
                if (unicode && ((input->size() % sizeof(wchar_t)) != 0))
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const UINT code_page = static_cast<UINT>(state.output_code_page());
                auto exe_decoded = decode_console_string(
                    unicode,
                    std::span<const std::byte>(input->data(), input->size()),
                    code_page,
                    L"ConsolepGetAliasesLength exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto exe_norm = fold_to_lower_invariant(*exe_decoded, L"ConsolepGetAliasesLength exe name fold failed");
                if (!exe_norm)
                {
                    message.set_reply_status(exe_norm.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                size_t total_bytes = 0;
                bool conversion_failed = false;

                state.for_each_alias(exe_norm.value(), [&](const std::wstring_view source, const std::wstring_view target) noexcept {
                    if (conversion_failed)
                    {
                        return;
                    }

                    if (unicode)
                    {
                        const size_t required = (source.size() + 1 + target.size() + 1) * sizeof(wchar_t);
                        total_bytes += required;
                        return;
                    }

                    if (source.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                        target.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                    {
                        conversion_failed = true;
                        return;
                    }

                    const int source_bytes = ::WideCharToMultiByte(
                        code_page,
                        0,
                        source.data(),
                        static_cast<int>(source.size()),
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                    if (source_bytes <= 0)
                    {
                        conversion_failed = true;
                        return;
                    }

                    const int target_bytes = ::WideCharToMultiByte(
                        code_page,
                        0,
                        target.data(),
                        static_cast<int>(target.size()),
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                    if (target_bytes <= 0)
                    {
                        conversion_failed = true;
                        return;
                    }

                    total_bytes += static_cast<size_t>(source_bytes);
                    total_bytes += 1; // '='
                    total_bytes += static_cast<size_t>(target_bytes);
                    total_bytes += 1; // null terminator
                });

                if (conversion_failed)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.AliasesLength =
                    total_bytes > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(total_bytes);

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetAliases))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasesW;
                body.AliasesBufferLength = 0;

                const bool unicode = body.Unicode != FALSE;
                if (unicode && ((input->size() % sizeof(wchar_t)) != 0))
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const UINT code_page = static_cast<UINT>(state.output_code_page());
                auto exe_decoded = decode_console_string(
                    unicode,
                    std::span<const std::byte>(input->data(), input->size()),
                    code_page,
                    L"ConsolepGetAliases exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto exe_norm = fold_to_lower_invariant(*exe_decoded, L"ConsolepGetAliases exe name fold failed");
                if (!exe_norm)
                {
                    message.set_reply_status(exe_norm.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                size_t written = 0;
                bool overflow = false;
                bool conversion_failed = false;

                state.for_each_alias(exe_norm.value(), [&](const std::wstring_view source, const std::wstring_view target) noexcept {
                    if (overflow || conversion_failed)
                    {
                        return;
                    }

                    if (unicode)
                    {
                        const size_t bytes_required = (source.size() + 1 + target.size() + 1) * sizeof(wchar_t);
                        if (output->size() - written < bytes_required)
                        {
                            overflow = true;
                            return;
                        }

                        auto* dest = reinterpret_cast<wchar_t*>(output->data() + written);
                        if (!source.empty())
                        {
                            std::memcpy(dest, source.data(), source.size() * sizeof(wchar_t));
                        }
                        dest += source.size();
                        *dest++ = L'=';
                        if (!target.empty())
                        {
                            std::memcpy(dest, target.data(), target.size() * sizeof(wchar_t));
                        }
                        dest += target.size();
                        *dest++ = L'\0';

                        written += bytes_required;
                        return;
                    }

                    if (source.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
                        target.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                    {
                        conversion_failed = true;
                        return;
                    }

                    const int source_bytes = ::WideCharToMultiByte(
                        code_page,
                        0,
                        source.data(),
                        static_cast<int>(source.size()),
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                    if (source_bytes <= 0)
                    {
                        conversion_failed = true;
                        return;
                    }

                    const int target_bytes = ::WideCharToMultiByte(
                        code_page,
                        0,
                        target.data(),
                        static_cast<int>(target.size()),
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                    if (target_bytes <= 0)
                    {
                        conversion_failed = true;
                        return;
                    }

                    const size_t bytes_required = static_cast<size_t>(source_bytes) + 1 + static_cast<size_t>(target_bytes) + 1;
                    if (output->size() - written < bytes_required)
                    {
                        overflow = true;
                        return;
                    }

                    auto* dest = reinterpret_cast<char*>(output->data() + written);
                    if (source_bytes != 0)
                    {
                        const int converted_source = ::WideCharToMultiByte(
                            code_page,
                            0,
                            source.data(),
                            static_cast<int>(source.size()),
                            dest,
                            source_bytes,
                            nullptr,
                            nullptr);
                        if (converted_source != source_bytes)
                        {
                            conversion_failed = true;
                            return;
                        }
                    }
                    dest += source_bytes;
                    *dest++ = '=';
                    if (target_bytes != 0)
                    {
                        const int converted_target = ::WideCharToMultiByte(
                            code_page,
                            0,
                            target.data(),
                            static_cast<int>(target.size()),
                            dest,
                            target_bytes,
                            nullptr,
                            nullptr);
                        if (converted_target != target_bytes)
                        {
                            conversion_failed = true;
                            return;
                        }
                    }
                    dest += target_bytes;
                    *dest++ = '\0';

                    written += bytes_required;
                });

                if (conversion_failed)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (overflow)
                {
                    message.set_reply_status(core::status_buffer_too_small);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.AliasesBufferLength =
                    written > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(written);

                message.set_reply_status(core::status_success);
                message.set_reply_information(body.AliasesBufferLength);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetAliasExesLength))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasExesLengthW;
                body.AliasExesLength = 0;

                const bool unicode = body.Unicode != FALSE;
                const UINT code_page = static_cast<UINT>(state.output_code_page());

                size_t total_bytes = 0;
                bool conversion_failed = false;

                state.for_each_alias_exe([&](const std::wstring_view exe_name) noexcept {
                    if (conversion_failed)
                    {
                        return;
                    }

                    if (unicode)
                    {
                        total_bytes += (exe_name.size() + 1) * sizeof(wchar_t);
                        return;
                    }

                    if (exe_name.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                    {
                        conversion_failed = true;
                        return;
                    }

                    const int bytes_required = ::WideCharToMultiByte(
                        code_page,
                        0,
                        exe_name.data(),
                        static_cast<int>(exe_name.size()),
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                    if (bytes_required <= 0)
                    {
                        conversion_failed = true;
                        return;
                    }

                    total_bytes += static_cast<size_t>(bytes_required) + 1;
                });

                if (conversion_failed)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.AliasExesLength =
                    total_bytes > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(total_bytes);

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetAliasExes))
            {
                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleAliasExesW;
                body.AliasExesBufferLength = 0;

                const bool unicode = body.Unicode != FALSE;
                const UINT code_page = static_cast<UINT>(state.output_code_page());

                size_t written = 0;
                bool overflow = false;
                bool conversion_failed = false;

                state.for_each_alias_exe([&](const std::wstring_view exe_name) noexcept {
                    if (overflow || conversion_failed)
                    {
                        return;
                    }

                    if (unicode)
                    {
                        const size_t bytes_required = (exe_name.size() + 1) * sizeof(wchar_t);
                        if (output->size() - written < bytes_required)
                        {
                            overflow = true;
                            return;
                        }

                        auto* dest = reinterpret_cast<wchar_t*>(output->data() + written);
                        if (!exe_name.empty())
                        {
                            std::memcpy(dest, exe_name.data(), exe_name.size() * sizeof(wchar_t));
                        }
                        dest += exe_name.size();
                        *dest++ = L'\0';

                        written += bytes_required;
                        return;
                    }

                    if (exe_name.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                    {
                        conversion_failed = true;
                        return;
                    }

                    const int name_bytes = ::WideCharToMultiByte(
                        code_page,
                        0,
                        exe_name.data(),
                        static_cast<int>(exe_name.size()),
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                    if (name_bytes <= 0)
                    {
                        conversion_failed = true;
                        return;
                    }

                    const size_t bytes_required = static_cast<size_t>(name_bytes) + 1;
                    if (output->size() - written < bytes_required)
                    {
                        overflow = true;
                        return;
                    }

                    auto* dest = reinterpret_cast<char*>(output->data() + written);
                    if (name_bytes != 0)
                    {
                        const int converted = ::WideCharToMultiByte(
                            code_page,
                            0,
                            exe_name.data(),
                            static_cast<int>(exe_name.size()),
                            dest,
                            name_bytes,
                            nullptr,
                            nullptr);
                        if (converted != name_bytes)
                        {
                            conversion_failed = true;
                            return;
                        }
                    }
                    dest += name_bytes;
                    *dest++ = '\0';

                    written += bytes_required;
                });

                if (conversion_failed)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                if (overflow)
                {
                    message.set_reply_status(core::status_buffer_too_small);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.AliasExesBufferLength =
                    written > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(written);

                message.set_reply_status(core::status_success);
                message.set_reply_information(body.AliasExesBufferLength);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetHistory))
            {
                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleHistory;
                body.HistoryBufferSize = state.history_buffer_size();
                body.NumberOfHistoryBuffers = state.history_buffer_count();
                body.dwFlags = state.history_flags();

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetHistory))
            {
                const auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleHistory;
                state.set_history_info(body.HistoryBufferSize, body.NumberOfHistoryBuffers, body.dwFlags);

                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepExpungeCommandHistory))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                const auto& body = packet.payload.user_defined.u.console_msg_l3.ExpungeConsoleCommandHistoryW;
                const bool unicode = body.Unicode != FALSE;
                const UINT code_page = static_cast<UINT>(state.output_code_page());

                auto exe_decoded = decode_console_string(
                    unicode,
                    std::span<const std::byte>(input->data(), input->size()),
                    code_page,
                    L"ConsolepExpungeCommandHistory exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                while (!exe_decoded->empty() && exe_decoded->back() == L'\0')
                {
                    exe_decoded->pop_back();
                }

                state.expunge_command_history(*exe_decoded);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepSetNumberOfCommands))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                const auto& body = packet.payload.user_defined.u.console_msg_l3.SetConsoleNumberOfCommandsW;
                const bool unicode = body.Unicode != FALSE;
                const UINT code_page = static_cast<UINT>(state.output_code_page());

                auto exe_decoded = decode_console_string(
                    unicode,
                    std::span<const std::byte>(input->data(), input->size()),
                    code_page,
                    L"ConsolepSetNumberOfCommands exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                while (!exe_decoded->empty() && exe_decoded->back() == L'\0')
                {
                    exe_decoded->pop_back();
                }

                state.set_command_history_number_of_commands(*exe_decoded, static_cast<size_t>(body.NumCommands));
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetCommandHistoryLength))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleCommandHistoryLengthW;
                const bool unicode = body.Unicode != FALSE;
                const UINT code_page = static_cast<UINT>(state.output_code_page());

                auto exe_decoded = decode_console_string(
                    unicode,
                    std::span<const std::byte>(input->data(), input->size()),
                    code_page,
                    L"ConsolepGetCommandHistoryLength exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                while (!exe_decoded->empty() && exe_decoded->back() == L'\0')
                {
                    exe_decoded->pop_back();
                }

                size_t length_bytes = 0;
                if (const auto* history = state.try_command_history_for_exe(*exe_decoded))
                {
                    for (const auto& command : history->commands())
                    {
                        size_t entry_bytes = 0;
                        if (unicode)
                        {
                            const size_t units = command.size() + 1;
                            if (units > (std::numeric_limits<size_t>::max() / sizeof(wchar_t)))
                            {
                                message.set_reply_status(core::status_invalid_parameter);
                                message.set_reply_information(0);
                                return outcome;
                            }
                            entry_bytes = units * sizeof(wchar_t);
                        }
                        else
                        {
                            const int required = ::WideCharToMultiByte(
                                code_page,
                                0,
                                command.data(),
                                static_cast<int>(command.size()),
                                nullptr,
                                0,
                                nullptr,
                                nullptr);
                            if (required <= 0)
                            {
                                message.set_reply_status(core::status_invalid_parameter);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            entry_bytes = static_cast<size_t>(required) + 1;
                        }

                        if (length_bytes > std::numeric_limits<size_t>::max() - entry_bytes)
                        {
                            message.set_reply_status(core::status_invalid_parameter);
                            message.set_reply_information(0);
                            return outcome;
                        }
                        length_bytes += entry_bytes;
                    }
                }

                if (length_bytes > static_cast<size_t>(std::numeric_limits<ULONG>::max()))
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.CommandHistoryLength = static_cast<ULONG>(length_bytes);
                message.set_reply_status(core::status_success);
                message.set_reply_information(0);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetCommandHistory))
            {
                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l3.GetConsoleCommandHistoryW;
                const bool unicode = body.Unicode != FALSE;
                const UINT code_page = static_cast<UINT>(state.output_code_page());

                auto exe_decoded = decode_console_string(
                    unicode,
                    std::span<const std::byte>(input->data(), input->size()),
                    code_page,
                    L"ConsolepGetCommandHistory exe name decode failed");
                if (!exe_decoded)
                {
                    message.set_reply_status(exe_decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                while (!exe_decoded->empty() && exe_decoded->back() == L'\0')
                {
                    exe_decoded->pop_back();
                }

                const auto* history = state.try_command_history_for_exe(*exe_decoded);

                size_t bytes_written = 0;
                if (unicode)
                {
                    if ((output->size() % sizeof(wchar_t)) != 0)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    auto* dest = reinterpret_cast<wchar_t*>(output->data());
                    const size_t capacity_units = output->size() / sizeof(wchar_t);
                    size_t written_units = 0;

                    if (history != nullptr)
                    {
                        for (const auto& command : history->commands())
                        {
                            const size_t needed = command.size() + 1;
                            if (written_units + needed > capacity_units)
                            {
                                message.set_reply_status(core::status_buffer_too_small);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            if (!command.empty())
                            {
                                std::memcpy(dest + written_units, command.data(), command.size() * sizeof(wchar_t));
                            }
                            dest[written_units + command.size()] = L'\0';
                            written_units += needed;
                        }
                    }

                    bytes_written = written_units * sizeof(wchar_t);
                }
                else
                {
                    auto* dest = reinterpret_cast<char*>(output->data());
                    const size_t capacity = output->size();

                    if (history != nullptr)
                    {
                        for (const auto& command : history->commands())
                        {
                            const int required = ::WideCharToMultiByte(
                                code_page,
                                0,
                                command.data(),
                                static_cast<int>(command.size()),
                                nullptr,
                                0,
                                nullptr,
                                nullptr);
                            if (required <= 0)
                            {
                                message.set_reply_status(core::status_invalid_parameter);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            const size_t bytes_required = static_cast<size_t>(required);
                            const size_t needed = bytes_required + 1;
                            if (bytes_written + needed > capacity)
                            {
                                message.set_reply_status(core::status_buffer_too_small);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            if (bytes_required != 0)
                            {
                                const int converted = ::WideCharToMultiByte(
                                    code_page,
                                    0,
                                    command.data(),
                                    static_cast<int>(command.size()),
                                    dest + bytes_written,
                                    required,
                                    nullptr,
                                    nullptr);
                                if (converted != required)
                                {
                                    message.set_reply_status(core::status_invalid_parameter);
                                    message.set_reply_information(0);
                                    return outcome;
                                }
                            }

                            dest[bytes_written + bytes_required] = '\0';
                            bytes_written += needed;
                        }
                    }
                }

                if (bytes_written > static_cast<size_t>(std::numeric_limits<ULONG>::max()))
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                body.CommandBufferLength = static_cast<ULONG>(bytes_written);
                message.set_reply_status(core::status_success);
                message.set_reply_information(body.CommandBufferLength);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepGetConsoleInput))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::input)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l1.GetConsoleInput;
                body.NumRecords = 0;

                if ((body.Flags & static_cast<USHORT>(~CONSOLE_READ_VALID)) != 0)
                {
                    message.set_reply_status(core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }

                const size_t capacity = output->size() / sizeof(INPUT_RECORD);
                if (capacity == 0)
                {
                    message.set_reply_status(core::status_success);
                    message.set_reply_information(0);
                    return outcome;
                }

                const bool is_peek = (body.Flags & CONSOLE_READ_NOREMOVE) != 0;
                const bool wait_allowed = (body.Flags & CONSOLE_READ_NOWAIT) == 0;
                const bool processed_input = (state.input_mode() & ENABLE_PROCESSED_INPUT) != 0;

                // `ConsolepGetConsoleInput` models input as a byte stream. UTF-8/DBCS sequences can be split
                // across reads, so the queue can temporarily contain bytes that cannot be decoded into an
                // `INPUT_RECORD` yet. We drain such prefixes into a per-handle buffer so reply-pending reads
                // can resume once more bytes arrive without leaving an undecodable prefix in the shared queue.
                auto& pending_prefix = handle->pending_input_bytes;

                const auto forward_ctrl_c = [&]() noexcept {
                    state.for_each_process([&](const ProcessState& process) noexcept {
                        (void)host_io.send_end_task(
                            process.pid,
                            CTRL_C_EVENT,
                            static_cast<DWORD>(core::console_ctrl_c_flag));
                    });
                };

                const auto forward_ctrl_break = [&]() noexcept {
                    state.for_each_process([&](const ProcessState& process) noexcept {
                        (void)host_io.send_end_task(
                            process.pid,
                            CTRL_BREAK_EVENT,
                            static_cast<DWORD>(core::console_ctrl_break_flag));
                    });
                };

                // In processed input mode Ctrl+C is a control event, not an input record.
                // If it is at the front of the byte queue, consume it immediately so
                // it never appears in peek/remove reads.
                if (processed_input)
                {
                    for (;;)
                    {
                        if (host_io.input_bytes_available() == 0)
                        {
                            break;
                        }

                        std::array<std::byte, 1> first{};
                        auto peeked = host_io.peek_input_bytes(first);
                        if (!peeked)
                        {
                            return std::unexpected(peeked.error());
                        }

                        if (peeked.value() != 1 || first[0] != static_cast<std::byte>(0x03))
                        {
                            break;
                        }

                        auto removed = host_io.read_input_bytes(first);
                        if (!removed)
                        {
                            return std::unexpected(removed.error());
                        }

                        if (removed.value() != 1)
                        {
                            break;
                        }

                        // Ctrl+C is processed at input time in the inbox host. Our
                        // byte-stream model forwards it when we observe it to avoid
                        // leaving stale control bytes in the queue.
                        forward_ctrl_c();
                    }
                }

                std::vector<std::byte> input_bytes;

                auto* records = reinterpret_cast<INPUT_RECORD*>(output->data());
                const UINT code_page = static_cast<UINT>(state.input_code_page());

                size_t records_written = 0;
                size_t bytes_consumed = 0;
                size_t ctrl_c_count = 0;
                size_t ctrl_break_count = 0;

                for (;;)
                {
                    records_written = 0;
                    bytes_consumed = 0;
                    ctrl_c_count = 0;
                    ctrl_break_count = 0;

                    if (wait_allowed &&
                        !handle->decoded_input_pending &&
                        pending_prefix.empty() &&
                        host_io.input_bytes_available() == 0)
                    {
                        if (host_io.input_disconnected())
                        {
                            message.set_reply_status(core::status_unsuccessful);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        outcome.reply_pending = true;
                        return outcome;
                    }

                    const size_t available_bytes = host_io.input_bytes_available();
                    constexpr size_t peek_limit = 64 * 1024;
                    constexpr size_t max_bytes_per_token = 64;
                    const size_t max_needed = capacity > (std::numeric_limits<size_t>::max() / max_bytes_per_token)
                        ? std::numeric_limits<size_t>::max()
                        : capacity * max_bytes_per_token;
                    const size_t to_peek = std::min(available_bytes, std::min(max_needed, peek_limit));

                    const size_t pending_byte_count = pending_prefix.size();

                    try
                    {
                        input_bytes.resize(pending_byte_count + to_peek);
                    }
                    catch (...)
                    {
                        message.set_reply_status(core::status_no_memory);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    if (pending_byte_count != 0)
                    {
                        const auto prefix = pending_prefix.bytes();
                        std::memcpy(input_bytes.data(), prefix.data(), pending_byte_count);
                    }

                    size_t queue_byte_count = 0;
                    if (to_peek != 0)
                    {
                        auto read = host_io.peek_input_bytes(std::span<std::byte>(input_bytes.data() + pending_byte_count, to_peek));
                        if (!read)
                        {
                            return std::unexpected(read.error());
                        }
                        queue_byte_count = read.value();
                    }

                    const size_t byte_count = pending_byte_count + queue_byte_count;

                    size_t offset = 0;

                    if (handle->decoded_input_pending)
                    {
                        const wchar_t value = *handle->decoded_input_pending;
                        const auto key = make_simple_character_key_event(value);
                        records[records_written] = make_input_record_from_key(key, body.Unicode != FALSE);
                        ++records_written;

                        if (!is_peek)
                        {
                            handle->decoded_input_pending.reset();
                        }
                    }

                    while (records_written < capacity && offset < byte_count)
                    {
                        vt_input::DecodedToken token{};
                        const auto outcome = decode_one_input_token(
                            code_page,
                            std::span<const std::byte>(input_bytes.data() + offset, byte_count - offset),
                            token);
                        if (outcome == InputDecodeOutcome::need_more_data)
                        {
                            break;
                        }

                        if (token.bytes_consumed == 0)
                        {
                            break;
                        }

                        if (token.kind == vt_input::TokenKind::ignored_sequence)
                        {
                            offset += token.bytes_consumed;
                            bytes_consumed = offset;
                            continue;
                        }

                        if (token.kind == vt_input::TokenKind::key_event)
                        {
                            if (processed_input && key_event_matches_ctrl_break(token.key))
                            {
                                offset += token.bytes_consumed;
                                bytes_consumed = offset;

                                if (token.key.bKeyDown)
                                {
                                    // Ctrl+Break flushes the input buffer and is not delivered as an input record.
                                    ++ctrl_break_count;
                                    records_written = 0;
                                    break;
                                }

                                continue;
                            }

                            if (processed_input && key_event_matches_ctrl_c(token.key))
                            {
                                if (token.key.bKeyDown)
                                {
                                    ++ctrl_c_count;
                                }
                                offset += token.bytes_consumed;
                                bytes_consumed = offset;
                                continue;
                            }

                            records[records_written] = make_input_record_from_key(token.key, body.Unicode != FALSE);
                            ++records_written;
                            offset += token.bytes_consumed;
                            bytes_consumed = offset;
                            continue;
                        }

                        const auto& text = token.text;
                        if (text.bytes_consumed == 0 || text.char_count == 0)
                        {
                            break;
                        }

                        if (processed_input && text.char_count == 1 && text.chars[0] == static_cast<wchar_t>(0x0003))
                        {
                            ++ctrl_c_count;
                            offset += token.bytes_consumed;
                            bytes_consumed = offset;
                            continue;
                        }

                        const size_t remaining_capacity = capacity - records_written;
                        if (text.char_count > remaining_capacity)
                        {
                            if (text.char_count == 2 && remaining_capacity == 1)
                            {
                                const auto key = make_simple_character_key_event(text.chars[0]);
                                records[records_written] = make_input_record_from_key(key, body.Unicode != FALSE);
                                ++records_written;

                                if (!is_peek)
                                {
                                    handle->decoded_input_pending = text.chars[1];
                                    offset += token.bytes_consumed;
                                    bytes_consumed = offset;
                                }
                            }

                            break;
                        }

                        for (size_t i = 0; i < text.char_count; ++i)
                        {
                            const auto key = make_simple_character_key_event(text.chars[i]);
                            records[records_written + i] = make_input_record_from_key(key, body.Unicode != FALSE);
                        }

                        records_written += text.char_count;
                        offset += token.bytes_consumed;
                        bytes_consumed = offset;
                    }

                    if (!is_peek && wait_allowed && records_written == 0 && bytes_consumed == 0)
                    {
                        if (host_io.input_disconnected())
                        {
                            message.set_reply_status(core::status_unsuccessful);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        if (byte_count == 0)
                        {
                            outcome.reply_pending = true;
                            return outcome;
                        }

                        vt_input::DecodedToken head{};
                        const auto head_outcome = decode_one_input_token(
                            code_page,
                            std::span<const std::byte>(input_bytes.data(), byte_count),
                            head);
                        if (head_outcome == InputDecodeOutcome::need_more_data)
                        {
                            if (queue_byte_count != 0)
                            {
                                const size_t pending_before = pending_prefix.size();
                                const auto drained = std::span<const std::byte>(
                                    input_bytes.data() + pending_before,
                                    queue_byte_count);
                                if (pending_prefix.append(drained))
                                {
                                    size_t remaining_to_discard = queue_byte_count;
                                    std::array<std::byte, 16> discard{};
                                    while (remaining_to_discard != 0)
                                    {
                                        const size_t discard_count = std::min(remaining_to_discard, discard.size());
                                        auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), discard_count));
                                        if (!removed)
                                        {
                                            return std::unexpected(removed.error());
                                        }

                                        if (removed.value() == 0)
                                        {
                                            break;
                                        }

                                        remaining_to_discard -= removed.value();
                                    }
                                }
                            }

                            outcome.reply_pending = true;
                            return outcome;
                        }
                    }

                    if (!is_peek && bytes_consumed != 0)
                    {
                        const size_t pending_before = pending_prefix.size();
                        const size_t pending_consumed = std::min(bytes_consumed, pending_before);
                        pending_prefix.consume_prefix(pending_consumed);

                        size_t remaining_to_discard = bytes_consumed - pending_consumed;
                        std::array<std::byte, 256> discard{};
                        while (remaining_to_discard != 0)
                        {
                            const size_t chunk = std::min(remaining_to_discard, discard.size());
                            auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), chunk));
                            if (!removed)
                            {
                                return std::unexpected(removed.error());
                            }

                            if (removed.value() == 0)
                            {
                                break;
                            }

                            remaining_to_discard -= removed.value();
                        }
                    }

                    if (processed_input && ctrl_break_count != 0)
                    {
                        if (auto flushed = host_io.flush_input_buffer(); !flushed)
                        {
                            return std::unexpected(flushed.error());
                        }

                        handle->decoded_input_pending.reset();
                        pending_prefix.clear();
                        handle->cooked_read_pending.clear();
                        handle->cooked_line_in_progress.clear();

                        for (size_t i = 0; i < ctrl_break_count; ++i)
                        {
                            forward_ctrl_break();
                        }
                    }

                    if (!is_peek && ctrl_c_count != 0)
                    {
                        for (size_t i = 0; i < ctrl_c_count; ++i)
                        {
                            forward_ctrl_c();
                        }
                    }

                    if (!is_peek && wait_allowed && records_written == 0)
                    {
                        if (host_io.input_disconnected())
                        {
                            message.set_reply_status(core::status_unsuccessful);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        if (host_io.input_bytes_available() == 0 &&
                            pending_prefix.empty() &&
                            !handle->decoded_input_pending)
                        {
                            outcome.reply_pending = true;
                            return outcome;
                        }

                        if (bytes_consumed != 0)
                        {
                            // We consumed only ignored sequences/processed Ctrl+C markers. Retry now that the head changed.
                            continue;
                        }
                    }

                    break;
                }

                body.NumRecords = records_written > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                    ? std::numeric_limits<ULONG>::max()
                    : static_cast<ULONG>(records_written);

                message.set_reply_status(core::status_success);
                message.set_reply_information(static_cast<ULONG_PTR>(records_written * sizeof(INPUT_RECORD)));
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepWriteConsole))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::output)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto* screen_buffer = handle->screen_buffer.get();
                if (screen_buffer == nullptr)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto input = message.get_input_buffer();
                if (!input)
                {
                    return std::unexpected(input.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l1.WriteConsole;
                body.NumBytes = 0;

                // ConDrv's `WriteConsole` message is the primary path used by classic clients
                // (WriteConsoleW/A) to render text. We keep a minimal implementation that:
                // - forwards bytes to `host_io` as a best-effort "headless sink"
                // - updates the in-memory `ScreenBuffer` so subsequent read/output APIs can
                //   observe consistent state (cursor advancement, wrapping, basic control chars).
                //
                // The full inbox host implements extensive processing (tabs, backspace,
                // scrolling regions, fullwidth, output modes, etc.). This replacement
                // intentionally starts small and is expanded incrementally.

                std::wstring text_to_write;
                if (body.Unicode)
                {
                    if ((input->size() % sizeof(wchar_t)) != 0)
                    {
                        message.set_reply_status(core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    const auto wchar_count = static_cast<int>(input->size() / sizeof(wchar_t));
                    try
                    {
                        text_to_write.resize(static_cast<size_t>(wchar_count));
                    }
                    catch (...)
                    {
                        message.set_reply_status(core::status_no_memory);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    if (!text_to_write.empty())
                    {
                        std::memcpy(text_to_write.data(), input->data(), input->size());
                    }

                    const int required = ::WideCharToMultiByte(
                        CP_UTF8,
                        0,
                        text_to_write.data(),
                        wchar_count,
                        nullptr,
                        0,
                        nullptr,
                        nullptr);
                    if (required <= 0)
                    {
                        return std::unexpected(DeviceCommError{
                            .context = L"WideCharToMultiByte failed for console output",
                            .win32_error = ::GetLastError(),
                        });
                    }

                    std::vector<std::byte> utf8;
                    try
                    {
                        utf8.resize(static_cast<size_t>(required));
                    }
                    catch (...)
                    {
                        message.set_reply_status(core::status_no_memory);
                        message.set_reply_information(0);
                        return outcome;
                    }
                    const int converted = ::WideCharToMultiByte(
                        CP_UTF8,
                        0,
                        text_to_write.data(),
                        wchar_count,
                        reinterpret_cast<char*>(utf8.data()),
                        required,
                        nullptr,
                        nullptr);
                    if (converted != required)
                    {
                        return std::unexpected(DeviceCommError{
                            .context = L"WideCharToMultiByte produced unexpected length",
                            .win32_error = ::GetLastError(),
                        });
                    }

                    auto written = host_io.write_output_bytes(std::span<const std::byte>(utf8.data(), utf8.size()));
                    if (!written)
                    {
                        return std::unexpected(written.error());
                    }

                    // Mirror the internal conhost contract: NumBytes is the amount of
                    // UTF-16 bytes consumed/written, not the number of UTF-8 bytes emitted.
                    body.NumBytes = static_cast<ULONG>(input->size());
                }
                else
                {
                    const UINT code_page = static_cast<UINT>(state.output_code_page());
                    auto decoded = decode_console_string(false, std::span<const std::byte>(input->data(), input->size()), code_page, L"ConsolepWriteConsole ANSI decode failed");
                    if (!decoded)
                    {
                        message.set_reply_status(decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                        message.set_reply_information(0);
                        return outcome;
                    }
                    text_to_write = std::move(decoded.value());

                    auto written = host_io.write_output_bytes(*input);
                    if (!written)
                    {
                        return std::unexpected(written.error());
                    }

                    body.NumBytes = static_cast<ULONG>(written.value());
                }

                apply_text_to_screen_buffer(*screen_buffer, text_to_write, state.output_mode(), &state, &host_io);

                message.set_reply_status(core::status_success);
                message.set_reply_information(body.NumBytes);
                return outcome;
            }

            if (api_number == static_cast<ULONG>(ConsolepReadConsole))
            {
                const auto handle_id = descriptor.object;
                auto* handle = state.find_object(handle_id);
                if (handle == nullptr || handle->kind != ObjectKind::input)
                {
                    message.set_reply_status(core::status_invalid_handle);
                    message.set_reply_information(0);
                    return outcome;
                }

                auto output = message.get_output_buffer();
                if (!output)
                {
                    return std::unexpected(output.error());
                }

                auto& body = packet.payload.user_defined.u.console_msg_l1.ReadConsole;
                body.ControlKeyState = 0;
                body.NumBytes = 0;

                if (output->empty())
                {
                    message.set_reply_status(core::status_success);
                    message.set_reply_information(0);
                    return outcome;
                }

                const ULONG input_mode = state.input_mode();
                const bool line_input = (input_mode & ENABLE_LINE_INPUT) != 0;
                const bool echo_input = (input_mode & ENABLE_ECHO_INPUT) != 0;
                const bool processed_input = (input_mode & ENABLE_PROCESSED_INPUT) != 0;

                auto& pending_prefix = handle->pending_input_bytes;

                if (line_input)
                {
                    auto& pending = handle->cooked_read_pending;
                    auto& line = handle->cooked_line_in_progress;
                    auto& cursor = handle->cooked_line_cursor;
                    auto& insert_mode = handle->cooked_insert_mode;

                    const auto is_high_surrogate = [](const wchar_t value) noexcept -> bool {
                        return value >= 0xD800 && value <= 0xDBFF;
                    };
                    const auto is_low_surrogate = [](const wchar_t value) noexcept -> bool {
                        return value >= 0xDC00 && value <= 0xDFFF;
                    };

                    const auto normalize_cursor = [&]() noexcept {
                        if (cursor > line.size())
                        {
                            cursor = line.size();
                        }

                        // Avoid leaving the cursor inside a surrogate pair.
                        if (cursor != 0 && cursor < line.size() &&
                            is_low_surrogate(line[cursor]) && is_high_surrogate(line[cursor - 1]))
                        {
                            --cursor;
                        }
                    };
                    normalize_cursor();

                    const auto deliver_pending = [&]() noexcept -> bool {
                        if (pending.empty())
                        {
                            body.NumBytes = 0;
                            message.set_reply_status(core::status_success);
                            message.set_reply_information(0);
                            return true;
                        }

                        if (body.Unicode)
                        {
                            const size_t max_wchars = output->size() / sizeof(wchar_t);
                            const size_t to_copy = std::min(pending.size(), max_wchars);
                            if (to_copy != 0)
                            {
                                std::memcpy(output->data(), pending.data(), to_copy * sizeof(wchar_t));
                                pending.erase(0, to_copy);
                            }

                            const size_t bytes_out = to_copy * sizeof(wchar_t);
                            body.NumBytes = bytes_out > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                                ? std::numeric_limits<ULONG>::max()
                                : static_cast<ULONG>(bytes_out);
                        }
                        else
                        {
                            const UINT code_page = static_cast<UINT>(state.input_code_page());
                            const auto* data = pending.data();
                            const size_t unit_count = pending.size();
                            const size_t capacity = output->size();

                            if (unit_count != 0 && capacity != 0)
                            {
                                const int max_units = unit_count > static_cast<size_t>(std::numeric_limits<int>::max())
                                    ? std::numeric_limits<int>::max()
                                    : static_cast<int>(unit_count);

                                int low = 0;
                                int high = max_units;
                                int best = 0;
                                while (low <= high)
                                {
                                    const int mid = low + ((high - low) / 2);
                                    const int required = ::WideCharToMultiByte(
                                        code_page,
                                        0,
                                        data,
                                        mid,
                                        nullptr,
                                        0,
                                        nullptr,
                                        nullptr);
                                    if (required <= 0)
                                    {
                                        high = mid - 1;
                                        continue;
                                    }

                                    const size_t required_size = static_cast<size_t>(required);
                                    if (required_size <= capacity)
                                    {
                                        best = mid;
                                        low = mid + 1;
                                    }
                                    else
                                    {
                                        high = mid - 1;
                                    }
                                }

                                if (best != 0 && static_cast<size_t>(best) < unit_count)
                                {
                                    if (is_high_surrogate(data[static_cast<size_t>(best) - 1]) &&
                                        is_low_surrogate(data[static_cast<size_t>(best)]))
                                    {
                                        --best;
                                    }
                                    else if (is_high_surrogate(data[static_cast<size_t>(best) - 1]))
                                    {
                                        --best;
                                    }
                                }

                                if (best == 0)
                                {
                                    // The caller provided a buffer that cannot hold even one encoded
                                    // character (e.g., UTF-8 multibyte sequences). Treat this as
                                    // a buffer-too-small error to avoid returning success with 0
                                    // bytes while leaving pending data intact. If the code page
                                    // is invalid, report invalid parameter instead.
                                    int minimal_units = 1;
                                    if (unit_count >= 2 &&
                                        is_high_surrogate(data[0]) &&
                                        is_low_surrogate(data[1]))
                                    {
                                        minimal_units = 2;
                                    }

                                    const int required = ::WideCharToMultiByte(
                                        code_page,
                                        0,
                                        data,
                                        minimal_units,
                                        nullptr,
                                        0,
                                        nullptr,
                                        nullptr);

                                    body.NumBytes = 0;
                                    message.set_reply_status(required <= 0 ? core::status_invalid_parameter : core::status_buffer_too_small);
                                    message.set_reply_information(0);
                                    return false;
                                }

                                if (best != 0)
                                {
                                    const int written = ::WideCharToMultiByte(
                                        code_page,
                                        0,
                                        data,
                                        best,
                                        reinterpret_cast<char*>(output->data()),
                                        capacity > static_cast<size_t>(std::numeric_limits<int>::max())
                                            ? std::numeric_limits<int>::max()
                                            : static_cast<int>(capacity),
                                        nullptr,
                                        nullptr);
                                    if (written <= 0)
                                    {
                                        message.set_reply_status(core::status_invalid_parameter);
                                        message.set_reply_information(0);
                                        return false;
                                    }

                                    body.NumBytes = static_cast<ULONG>(written);
                                    pending.erase(0, static_cast<size_t>(best));
                                }
                            }
                        }

                        message.set_reply_status(core::status_success);
                        message.set_reply_information(body.NumBytes);
                        return true;
                    };

                    if (!pending.empty())
                    {
                        (void)deliver_pending();
                        return outcome;
                    }

                    const auto echo_text = [&](const std::wstring_view value) noexcept -> std::expected<void, DeviceCommError> {
                        if (!echo_input || value.empty())
                        {
                            return {};
                        }

                        if (auto screen_buffer = state.active_screen_buffer())
                        {
                            apply_text_to_screen_buffer(*screen_buffer, value, state.output_mode(), &state, &host_io);
                        }

                        if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
                        {
                            return std::unexpected(DeviceCommError{
                                .context = L"ReadConsole echo exceeded WideCharToMultiByte limits",
                                .win32_error = ERROR_INVALID_DATA,
                            });
                        }

                        const int required = ::WideCharToMultiByte(
                            CP_UTF8,
                            0,
                            value.data(),
                            static_cast<int>(value.size()),
                            nullptr,
                            0,
                            nullptr,
                            nullptr);
                        if (required <= 0)
                        {
                            return std::unexpected(DeviceCommError{
                                .context = L"WideCharToMultiByte failed for ReadConsole echo",
                                .win32_error = ::GetLastError(),
                            });
                        }

                        std::vector<std::byte> utf8;
                        try
                        {
                            utf8.resize(static_cast<size_t>(required));
                        }
                        catch (...)
                        {
                            return std::unexpected(DeviceCommError{
                                .context = L"ReadConsole echo allocation failed",
                                .win32_error = ERROR_OUTOFMEMORY,
                            });
                        }

                        const int converted = ::WideCharToMultiByte(
                            CP_UTF8,
                            0,
                            value.data(),
                            static_cast<int>(value.size()),
                            reinterpret_cast<char*>(utf8.data()),
                            required,
                            nullptr,
                            nullptr);
                        if (converted != required)
                        {
                            return std::unexpected(DeviceCommError{
                                .context = L"WideCharToMultiByte produced unexpected length for ReadConsole echo",
                                .win32_error = ::GetLastError(),
                            });
                        }

                        auto written = host_io.write_output_bytes(std::span<const std::byte>(utf8.data(), utf8.size()));
                        if (!written)
                        {
                            return std::unexpected(written.error());
                        }

                        return {};
                    };

                    if (line.empty())
                    {
                        try
                        {
                            line.reserve(64);
                        }
                        catch (...)
                        {
                            // Best-effort: cooked reads can proceed without reserving.
                        }
                    }

                    const UINT code_page = static_cast<UINT>(state.input_code_page());
                    constexpr std::wstring_view suffix_processed = L"\r\n";
                    constexpr std::wstring_view suffix_raw = L"\r";
                    const std::wstring_view newline_suffix = processed_input ? suffix_processed : suffix_raw;

                    const auto prev_index = [&](const size_t index) noexcept -> size_t {
                        if (index == 0)
                        {
                            return 0;
                        }

                        size_t prev = index - 1;
                        if (prev != 0 && is_low_surrogate(line[prev]) && is_high_surrogate(line[prev - 1]))
                        {
                            --prev;
                        }
                        return prev;
                    };

                    const auto next_index = [&](const size_t index) noexcept -> size_t {
                        if (index >= line.size())
                        {
                            return line.size();
                        }

                        size_t next = index + 1;
                        if (next < line.size() && is_high_surrogate(line[index]) && is_low_surrogate(line[index + 1]))
                        {
                            next = index + 2;
                        }
                        return next;
                    };

                    const auto is_word_delimiter = [&](const size_t index) noexcept -> bool {
                        if (index >= line.size())
                        {
                            return false;
                        }

                        const wchar_t ch = line[index];
                        return ch == L' ' || ch == L'\t';
                    };

                    // VT sequences (for example ConPTY win32-input-mode) and UTF-8/DBCS sequences can be
                    // split across reads. If we see an incomplete sequence at the head of the stream,
                    // drain the bytes from the shared queue into the per-handle prefix buffer and wait
                    // until more input arrives.

                    for (;;)
                    {
                        if (host_io.input_bytes_available() == 0 && pending_prefix.empty())
                        {
                            break;
                        }

                        std::array<std::byte, 64> peek{};
                        const size_t pending_before = pending_prefix.size();
                        OC_ASSERT(pending_before <= peek.size());
                        if (pending_before != 0)
                        {
                            const auto prefix = pending_prefix.bytes();
                            std::memcpy(peek.data(), prefix.data(), pending_before);
                        }

                        size_t peeked_bytes = 0;
                        if (pending_before < peek.size())
                        {
                            auto peeked = host_io.peek_input_bytes(std::span<std::byte>(peek.data() + pending_before, peek.size() - pending_before));
                            if (!peeked)
                            {
                                return std::unexpected(peeked.error());
                            }
                            peeked_bytes = peeked.value();
                        }
                        const size_t total_bytes = pending_before + peeked_bytes;
                        if (total_bytes == 0)
                        {
                            break;
                        }

                        vt_input::DecodedToken token{};
                        const auto decode_outcome = decode_one_input_token(
                            code_page,
                            std::span<const std::byte>(peek.data(), total_bytes),
                            token);
                        if (decode_outcome == InputDecodeOutcome::need_more_data)
                        {
                            if (peeked_bytes != 0)
                            {
                                const auto drained = std::span<const std::byte>(peek.data() + pending_before, peeked_bytes);
                                if (pending_prefix.append(drained))
                                {
                                    size_t remaining_to_discard = peeked_bytes;
                                    std::array<std::byte, 16> discard{};
                                    while (remaining_to_discard != 0)
                                    {
                                        const size_t discard_count = std::min(remaining_to_discard, discard.size());
                                        auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), discard_count));
                                        if (!removed)
                                        {
                                            return std::unexpected(removed.error());
                                        }

                                        if (removed.value() == 0)
                                        {
                                            break;
                                        }

                                        remaining_to_discard -= removed.value();
                                    }
                                }
                            }

                            break;
                        }

                        if (token.bytes_consumed == 0)
                        {
                            break;
                        }

                        const size_t pending_consumed = std::min(token.bytes_consumed, pending_before);
                        pending_prefix.consume_prefix(pending_consumed);

                        size_t remaining_to_discard = token.bytes_consumed - pending_consumed;
                        std::array<std::byte, 16> discard{};
                        while (remaining_to_discard != 0)
                        {
                            const size_t discard_count = std::min(remaining_to_discard, discard.size());
                            auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), discard_count));
                            if (!removed)
                            {
                                return std::unexpected(removed.error());
                            }

                            if (removed.value() == 0)
                            {
                                remaining_to_discard = 0;
                                break;
                            }

                            remaining_to_discard -= removed.value();
                        }

                        if (token.kind == vt_input::TokenKind::ignored_sequence)
                        {
                            // Focus/DA1 responses and other non-input control sequences are not
                            // cooked characters. They are consumed and ignored.
                            continue;
                        }

                        const auto echo_repeat = [&](const wchar_t ch, size_t count) noexcept -> std::expected<void, DeviceCommError> {
                            if (!echo_input || count == 0)
                            {
                                return {};
                            }

                            std::array<wchar_t, 64> buffer{};
                            buffer.fill(ch);
                            while (count != 0)
                            {
                                const size_t chunk = std::min(count, buffer.size());
                                if (auto echoed = echo_text(std::wstring_view(buffer.data(), chunk)); !echoed)
                                {
                                    return std::unexpected(echoed.error());
                                }
                                count -= chunk;
                            }

                            return {};
                        };

                        const auto echo_backspaces = [&](const size_t count) noexcept -> std::expected<void, DeviceCommError> {
                            return echo_repeat(L'\b', count);
                        };

                        const auto echo_spaces = [&](const size_t count) noexcept -> std::expected<void, DeviceCommError> {
                            return echo_repeat(L' ', count);
                        };

                        const auto echo_range = [&](const size_t from, const size_t to) noexcept -> std::expected<void, DeviceCommError> {
                            if (from >= to || to > line.size())
                            {
                                return {};
                            }

                            return echo_text(std::wstring_view(line.data() + from, to - from));
                        };

                        const auto handle_single_unit = [&](const wchar_t value) noexcept
                            -> std::expected<bool, DeviceCommError> {
                            normalize_cursor();
                            if (body.ProcessControlZ != FALSE && line.empty() && value == static_cast<wchar_t>(0x001A))
                            {
                                body.NumBytes = 0;
                                if (!output->empty())
                                {
                                    output->front() = std::byte{ 0 };
                                }
                                message.set_reply_status(core::status_success);
                                message.set_reply_information(0);
                                return true;
                            }

                            if (processed_input && value == static_cast<wchar_t>(0x0003))
                            {
                                // Mirror the inbox host: Ctrl+C terminates cooked reads with STATUS_ALERTED
                                // and is not delivered as input to the client.
                                state.for_each_process([&](const ProcessState& process) noexcept {
                                    (void)host_io.send_end_task(
                                        process.pid,
                                        CTRL_C_EVENT,
                                        static_cast<DWORD>(core::console_ctrl_c_flag));
                                });

                                body.NumBytes = 0;
                                message.set_reply_status(core::status_alerted);
                                message.set_reply_information(0);
                                return true;
                            }

                            if (value == L'\b')
                            {
                                if (cursor == 0)
                                {
                                    return false;
                                }

                                const size_t new_cursor = prev_index(cursor);
                                const size_t removed_units = cursor - new_cursor;
                                if (removed_units == 0)
                                {
                                    return false;
                                }

                                line.erase(new_cursor, removed_units);
                                cursor = new_cursor;
                                normalize_cursor();

                                if (auto echoed = echo_backspaces(removed_units); !echoed)
                                {
                                    return std::unexpected(echoed.error());
                                }

                                if (cursor < line.size())
                                {
                                    if (auto echoed = echo_range(cursor, line.size()); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }

                                    if (auto echoed = echo_spaces(removed_units); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }

                                    if (auto echoed = echo_backspaces((line.size() - cursor) + removed_units); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                }
                                else
                                {
                                    if (auto echoed = echo_spaces(removed_units); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }

                                    if (auto echoed = echo_backspaces(removed_units); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                }

                                return false;
                            }

                            if (value == L'\r' || value == L'\n')
                            {
                                if (cursor < line.size())
                                {
                                    if (auto echoed = echo_range(cursor, line.size()); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                    cursor = line.size();
                                }

                                if (value == L'\r' && (host_io.input_bytes_available() != 0 || !pending_prefix.empty()))
                                {
                                    std::array<std::byte, 64> lf_peek{};
                                    const size_t lf_pending_before = pending_prefix.size();
                                    OC_ASSERT(lf_pending_before <= lf_peek.size());
                                    if (lf_pending_before != 0)
                                    {
                                        const auto prefix = pending_prefix.bytes();
                                        std::memcpy(lf_peek.data(), prefix.data(), lf_pending_before);
                                    }

                                    size_t lf_peeked_bytes = 0;
                                    if (lf_pending_before < lf_peek.size())
                                    {
                                        auto lf_peeked = host_io.peek_input_bytes(
                                            std::span<std::byte>(lf_peek.data() + lf_pending_before, lf_peek.size() - lf_pending_before));
                                        if (!lf_peeked)
                                        {
                                            return std::unexpected(lf_peeked.error());
                                        }
                                        lf_peeked_bytes = lf_peeked.value();
                                    }

                                    const size_t lf_total = lf_pending_before + lf_peeked_bytes;
                                    if (lf_total != 0)
                                    {
                                        vt_input::DecodedToken lf_token{};
                                        if (decode_one_input_token(
                                                code_page,
                                                std::span<const std::byte>(lf_peek.data(), lf_total),
                                                lf_token) == InputDecodeOutcome::produced &&
                                            lf_token.bytes_consumed != 0)
                                        {
                                            bool is_lf = false;
                                            if (lf_token.kind == vt_input::TokenKind::text_units &&
                                                lf_token.text.char_count == 1 &&
                                                lf_token.text.chars[0] == L'\n')
                                            {
                                                is_lf = true;
                                            }
                                            else if (lf_token.kind == vt_input::TokenKind::key_event &&
                                                     lf_token.key.bKeyDown &&
                                                     lf_token.key.uChar.UnicodeChar == L'\n')
                                            {
                                                is_lf = true;
                                            }

                                            if (is_lf)
                                            {
                                                const size_t lf_pending_consumed = std::min(lf_token.bytes_consumed, lf_pending_before);
                                                pending_prefix.consume_prefix(lf_pending_consumed);

                                                size_t lf_remaining_to_discard = lf_token.bytes_consumed - lf_pending_consumed;
                                                std::array<std::byte, 16> lf_discard{};
                                                while (lf_remaining_to_discard != 0)
                                                {
                                                    const size_t discard_count = std::min(lf_remaining_to_discard, lf_discard.size());
                                                    auto removed = host_io.read_input_bytes(std::span<std::byte>(lf_discard.data(), discard_count));
                                                    if (!removed)
                                                    {
                                                        return std::unexpected(removed.error());
                                                    }

                                                    if (removed.value() == 0)
                                                    {
                                                        break;
                                                    }

                                                    lf_remaining_to_discard -= removed.value();
                                                }
                                            }
                                        }
                                    }
                                }

                                if (auto echoed = echo_text(newline_suffix); !echoed)
                                {
                                    return std::unexpected(echoed.error());
                                }

                                if (echo_input)
                                {
                                    const bool suppress_duplicates = (state.history_flags() & HISTORY_NO_DUP_FLAG) != 0;
                                    state.add_command_history_for_process(handle->owning_process, std::wstring_view(line), suppress_duplicates);
                                }

                                try
                                {
                                    line.append(newline_suffix);
                                }
                                catch (...)
                                {
                                    message.set_reply_status(core::status_no_memory);
                                    message.set_reply_information(0);
                                    return true;
                                }

                                pending = std::move(line);
                                cursor = 0;
                                (void)deliver_pending();
                                return true;
                            }

                            size_t removed_units = 0;
                            if (!insert_mode && cursor < line.size())
                            {
                                const size_t end = next_index(cursor);
                                removed_units = end - cursor;
                                if (removed_units != 0)
                                {
                                    line.erase(cursor, removed_units);
                                }
                            }

                            try
                            {
                                line.insert(cursor, 1, value);
                            }
                            catch (...)
                            {
                                message.set_reply_status(core::status_no_memory);
                                message.set_reply_information(0);
                                return true;
                            }

                            ++cursor;
                            normalize_cursor();

                            const size_t tail_units = line.size() - cursor;
                            if (auto echoed = echo_text(std::wstring_view(&value, 1)); !echoed)
                            {
                                return std::unexpected(echoed.error());
                            }

                            if (tail_units != 0)
                            {
                                if (auto echoed = echo_text(std::wstring_view(line.data() + cursor, tail_units)); !echoed)
                                {
                                    return std::unexpected(echoed.error());
                                }
                            }

                            const size_t clear_units = removed_units > 1 ? removed_units - 1 : 0;
                            if (clear_units != 0)
                            {
                                if (auto echoed = echo_spaces(clear_units); !echoed)
                                {
                                    return std::unexpected(echoed.error());
                                }
                            }

                            const size_t backspaces = tail_units + clear_units;
                            if (backspaces != 0)
                            {
                                if (auto echoed = echo_backspaces(backspaces); !echoed)
                                {
                                    return std::unexpected(echoed.error());
                                }
                            }
                            return false;
                        };

                        if (token.kind == vt_input::TokenKind::key_event)
                        {
                            const auto& key = token.key;
                            if (!key.bKeyDown)
                            {
                                continue;
                            }

                            if (processed_input && key_event_matches_ctrl_break(key))
                            {
                                // Mirror the inbox host: Ctrl+Break flushes the input buffer, generates
                                // a CTRL_BREAK_EVENT, and terminates cooked reads with STATUS_ALERTED.
                                if (auto flushed = host_io.flush_input_buffer(); !flushed)
                                {
                                    return std::unexpected(flushed.error());
                                }

                                handle->decoded_input_pending.reset();
                                pending_prefix.clear();
                                pending.clear();
                                line.clear();
                                cursor = 0;
                                insert_mode = true;

                                state.for_each_process([&](const ProcessState& process) noexcept {
                                    (void)host_io.send_end_task(
                                        process.pid,
                                        CTRL_BREAK_EVENT,
                                        static_cast<DWORD>(core::console_ctrl_break_flag));
                                });

                                body.NumBytes = 0;
                                message.set_reply_status(core::status_alerted);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            if (processed_input && key_event_matches_ctrl_c(key))
                            {
                                state.for_each_process([&](const ProcessState& process) noexcept {
                                    (void)host_io.send_end_task(
                                        process.pid,
                                        CTRL_C_EVENT,
                                        static_cast<DWORD>(core::console_ctrl_c_flag));
                                });

                                body.NumBytes = 0;
                                message.set_reply_status(core::status_alerted);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            const size_t repeat = std::max<size_t>(1, static_cast<size_t>(key.wRepeatCount));
                            const bool ctrl_pressed = (key.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                            const WORD vkey = key.wVirtualKeyCode;

                            const auto word_prev = [&](const size_t index) noexcept -> size_t {
                                size_t pos = index;
                                while (pos != 0)
                                {
                                    const size_t prev = prev_index(pos);
                                    if (!is_word_delimiter(prev))
                                    {
                                        break;
                                    }
                                    pos = prev;
                                }

                                while (pos != 0)
                                {
                                    const size_t prev = prev_index(pos);
                                    if (is_word_delimiter(prev))
                                    {
                                        break;
                                    }
                                    pos = prev;
                                }

                                return pos;
                            };

                            const auto word_next = [&](const size_t index) noexcept -> size_t {
                                size_t pos = index;
                                while (pos < line.size())
                                {
                                    if (is_word_delimiter(pos))
                                    {
                                        break;
                                    }
                                    pos = next_index(pos);
                                }

                                while (pos < line.size())
                                {
                                    if (!is_word_delimiter(pos))
                                    {
                                        break;
                                    }
                                    pos = next_index(pos);
                                }

                                return pos;
                            };

                            bool handled_edit_key = false;
                            switch (vkey)
                            {
                            case VK_INSERT:
                                handled_edit_key = true;
                                if ((repeat % 2) == 1)
                                {
                                    insert_mode = !insert_mode;
                                }
                                break;
                            case VK_ESCAPE:
                                handled_edit_key = true;
                                if (!line.empty())
                                {
                                    const size_t old_size = line.size();
                                    const size_t old_cursor = cursor;
                                    line.clear();
                                    cursor = 0;
                                    normalize_cursor();

                                    if (auto echoed = echo_backspaces(old_cursor); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                    if (auto echoed = echo_spaces(old_size); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                    if (auto echoed = echo_backspaces(old_size); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                }
                                break;
                            case VK_HOME:
                                handled_edit_key = true;
                                if (ctrl_pressed)
                                {
                                    const size_t removed_units = cursor;
                                    if (removed_units != 0)
                                    {
                                        line.erase(0, removed_units);
                                        cursor = 0;
                                        normalize_cursor();

                                        if (auto echoed = echo_backspaces(removed_units); !echoed)
                                        {
                                            return std::unexpected(echoed.error());
                                        }
                                        if (auto echoed = echo_range(0, line.size()); !echoed)
                                        {
                                            return std::unexpected(echoed.error());
                                        }
                                        if (auto echoed = echo_spaces(removed_units); !echoed)
                                        {
                                            return std::unexpected(echoed.error());
                                        }
                                        if (auto echoed = echo_backspaces(line.size() + removed_units); !echoed)
                                        {
                                            return std::unexpected(echoed.error());
                                        }
                                    }
                                }
                                else if (cursor != 0)
                                {
                                    const size_t moved = cursor;
                                    cursor = 0;
                                    normalize_cursor();
                                    if (auto echoed = echo_backspaces(moved); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                }
                                break;
                            case VK_END:
                                handled_edit_key = true;
                                if (ctrl_pressed)
                                {
                                    if (cursor < line.size())
                                    {
                                        const size_t removed_units = line.size() - cursor;
                                        line.erase(cursor, removed_units);
                                        normalize_cursor();

                                        if (auto echoed = echo_spaces(removed_units); !echoed)
                                        {
                                            return std::unexpected(echoed.error());
                                        }
                                        if (auto echoed = echo_backspaces(removed_units); !echoed)
                                        {
                                            return std::unexpected(echoed.error());
                                        }
                                    }
                                }
                                else if (cursor < line.size())
                                {
                                    if (auto echoed = echo_range(cursor, line.size()); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                    cursor = line.size();
                                    normalize_cursor();
                                }
                                break;
                            case VK_LEFT:
                                handled_edit_key = true;
                                for (size_t i = 0; i < repeat; ++i)
                                {
                                    if (cursor == 0)
                                    {
                                        break;
                                    }

                                    const size_t new_cursor = ctrl_pressed ? word_prev(cursor) : prev_index(cursor);
                                    const size_t moved = cursor - new_cursor;
                                    cursor = new_cursor;
                                    normalize_cursor();
                                    if (auto echoed = echo_backspaces(moved); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                }
                                break;
                            case VK_RIGHT:
                                handled_edit_key = true;
                                for (size_t i = 0; i < repeat; ++i)
                                {
                                    if (cursor >= line.size())
                                    {
                                        break;
                                    }

                                    const size_t new_cursor = ctrl_pressed ? word_next(cursor) : next_index(cursor);
                                    if (auto echoed = echo_range(cursor, new_cursor); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                    cursor = new_cursor;
                                    normalize_cursor();
                                }
                                break;
                            case VK_DELETE:
                                handled_edit_key = true;
                                for (size_t i = 0; i < repeat; ++i)
                                {
                                    if (cursor >= line.size())
                                    {
                                        break;
                                    }

                                    const size_t end = next_index(cursor);
                                    const size_t removed_units = end - cursor;
                                    if (removed_units == 0)
                                    {
                                        break;
                                    }

                                    line.erase(cursor, removed_units);
                                    normalize_cursor();

                                    if (auto echoed = echo_range(cursor, line.size()); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                    if (auto echoed = echo_spaces(removed_units); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                    if (auto echoed = echo_backspaces((line.size() - cursor) + removed_units); !echoed)
                                    {
                                        return std::unexpected(echoed.error());
                                    }
                                }
                                break;
                            default:
                                break;
                            }

                            if (handled_edit_key)
                            {
                                continue;
                            }

                            const wchar_t value = key.uChar.UnicodeChar;
                            if (value == L'\0')
                            {
                                continue;
                            }

                            for (size_t i = 0; i < repeat; ++i)
                            {
                                auto handled = handle_single_unit(value);
                                if (!handled)
                                {
                                    return std::unexpected(handled.error());
                                }
                                if (handled.value())
                                {
                                    return outcome;
                                }
                            }
                            continue;
                        }

                        const auto& chunk = token.text;
                        if (chunk.bytes_consumed == 0 || chunk.char_count == 0)
                        {
                            break;
                        }

                        if (chunk.char_count == 1)
                        {
                            auto handled = handle_single_unit(chunk.chars[0]);
                            if (!handled)
                            {
                                return std::unexpected(handled.error());
                            }
                            if (handled.value())
                            {
                                return outcome;
                            }
                            continue;
                        }

                        for (size_t i = 0; i < chunk.char_count; ++i)
                        {
                            auto handled = handle_single_unit(chunk.chars[i]);
                            if (!handled)
                            {
                                return std::unexpected(handled.error());
                            }
                            if (handled.value())
                            {
                                return outcome;
                            }
                        }
                    }

                    if (host_io.input_disconnected())
                    {
                        pending_prefix.clear();
                        line.clear();
                        cursor = 0;
                        message.set_reply_status(core::status_unsuccessful);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    outcome.reply_pending = true;
                    return outcome;
                }

                if (processed_input)
                {
                    // In processed input mode, Ctrl+C is handled by the host (it generates a CTRL_C_EVENT
                    // and is not delivered as input to the client). For raw reads this does not terminate
                    // the read; we simply consume the byte and continue waiting for real data.
                    for (;;)
                    {
                        if (host_io.input_bytes_available() == 0)
                        {
                            break;
                        }

                        std::array<std::byte, 1> first{};
                        auto peeked = host_io.peek_input_bytes(first);
                        if (!peeked)
                        {
                            return std::unexpected(peeked.error());
                        }

                        if (peeked.value() != 1 || first[0] != static_cast<std::byte>(0x03))
                        {
                            break;
                        }

                        auto removed = host_io.read_input_bytes(first);
                        if (!removed)
                        {
                            return std::unexpected(removed.error());
                        }

                        if (removed.value() != 1)
                        {
                            break;
                        }

                        state.for_each_process([&](const ProcessState& process) noexcept {
                            (void)host_io.send_end_task(
                                process.pid,
                                CTRL_C_EVENT,
                                static_cast<DWORD>(core::console_ctrl_c_flag));
                        });
                    }
                }

                if (host_io.input_bytes_available() == 0 && pending_prefix.empty())
                {
                    const bool has_pending_unit = (body.Unicode != FALSE) && handle->decoded_input_pending.has_value();
                    if (!has_pending_unit)
                    {
                        if (host_io.input_disconnected())
                        {
                            message.set_reply_status(core::status_unsuccessful);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        outcome.reply_pending = true;
                        return outcome;
                    }
                }

                if (body.ProcessControlZ != FALSE && host_io.input_bytes_available() != 0)
                {
                    std::array<std::byte, 1> first{};
                    auto peeked = host_io.peek_input_bytes(first);
                    if (!peeked)
                    {
                        return std::unexpected(peeked.error());
                    }

                    if (peeked.value() == 1 && first[0] == static_cast<std::byte>(0x1a))
                    {
                        auto removed = host_io.read_input_bytes(first);
                        if (!removed)
                        {
                            return std::unexpected(removed.error());
                        }

                        body.NumBytes = 0;
                        if (!output->empty())
                        {
                            output->front() = std::byte{ 0 };
                        }
                        message.set_reply_status(core::status_success);
                        message.set_reply_information(0);
                        return outcome;
                    }
                }

                if (body.Unicode)
                {
                    const size_t max_wchars = output->size() / sizeof(wchar_t);
                    if (max_wchars == 0)
                    {
                        message.set_reply_status(core::status_success);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    const UINT code_page = static_cast<UINT>(state.input_code_page());
                    auto* out_chars = reinterpret_cast<wchar_t*>(output->data());
                    size_t written_units = 0;

                    if (handle->decoded_input_pending && written_units < max_wchars)
                    {
                        out_chars[written_units] = *handle->decoded_input_pending;
                        handle->decoded_input_pending.reset();
                        ++written_units;
                    }

                    // UTF-8/DBCS sequences can be split across reads. If the head of the stream is an
                    // incomplete multibyte sequence and this read has not produced any output yet,
                    // drain it into the per-handle prefix buffer and reply-pend until more input arrives.

                    while (written_units < max_wchars)
                    {
                        if (host_io.input_bytes_available() == 0 && pending_prefix.empty())
                        {
                            if (written_units != 0)
                            {
                                break;
                            }

                            if (host_io.input_disconnected())
                            {
                                message.set_reply_status(core::status_unsuccessful);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            outcome.reply_pending = true;
                            return outcome;
                        }

                        std::array<std::byte, 64> peek{};
                        const size_t pending_before = pending_prefix.size();
                        OC_ASSERT(pending_before <= peek.size());
                        if (pending_before != 0)
                        {
                            const auto prefix = pending_prefix.bytes();
                            std::memcpy(peek.data(), prefix.data(), pending_before);
                        }

                        size_t peeked_bytes = 0;
                        if (pending_before < peek.size())
                        {
                            auto peeked = host_io.peek_input_bytes(std::span<std::byte>(peek.data() + pending_before, peek.size() - pending_before));
                            if (!peeked)
                            {
                                return std::unexpected(peeked.error());
                            }
                            peeked_bytes = peeked.value();
                        }

                        const size_t total_bytes = pending_before + peeked_bytes;
                        if (total_bytes == 0)
                        {
                            continue;
                        }

                        vt_input::DecodedToken token{};
                        const auto decode_outcome = decode_one_input_token(
                            code_page,
                            std::span<const std::byte>(peek.data(), total_bytes),
                            token);
                        if (decode_outcome == InputDecodeOutcome::need_more_data)
                        {
                            if (written_units != 0)
                            {
                                break;
                            }

                            if (peeked_bytes != 0)
                            {
                                const auto drained = std::span<const std::byte>(peek.data() + pending_before, peeked_bytes);
                                if (pending_prefix.append(drained))
                                {
                                    size_t remaining_to_discard = peeked_bytes;
                                    std::array<std::byte, 8> discard{};
                                    while (remaining_to_discard != 0)
                                    {
                                        const size_t discard_count = std::min(remaining_to_discard, discard.size());
                                        auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), discard_count));
                                        if (!removed)
                                        {
                                            return std::unexpected(removed.error());
                                        }

                                        if (removed.value() == 0)
                                        {
                                            break;
                                        }

                                        remaining_to_discard -= removed.value();
                                    }
                                }
                            }

                            if (host_io.input_disconnected())
                            {
                                message.set_reply_status(core::status_unsuccessful);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            outcome.reply_pending = true;
                            return outcome;
                        }

                        if (token.bytes_consumed == 0)
                        {
                            break;
                        }

                        const size_t remaining_units = max_wchars - written_units;
                        bool split_surrogate = false;
                        if (token.kind == vt_input::TokenKind::text_units && token.text.char_count > remaining_units)
                        {
                            if (token.text.char_count == 2 && remaining_units == 1)
                            {
                                split_surrogate = true;
                            }
                            else
                            {
                                break;
                            }
                        }

                        const size_t pending_consumed = std::min(token.bytes_consumed, pending_before);
                        pending_prefix.consume_prefix(pending_consumed);

                        size_t remaining_to_discard = token.bytes_consumed - pending_consumed;
                        std::array<std::byte, 16> discard{};
                        while (remaining_to_discard != 0)
                        {
                            const size_t discard_count = std::min(remaining_to_discard, discard.size());
                            auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), discard_count));
                            if (!removed)
                            {
                                return std::unexpected(removed.error());
                            }

                            if (removed.value() == 0)
                            {
                                remaining_to_discard = 0;
                                break;
                            }

                            remaining_to_discard -= removed.value();
                        }

                        if (token.kind == vt_input::TokenKind::ignored_sequence)
                        {
                            continue;
                        }

                        if (token.kind == vt_input::TokenKind::key_event)
                        {
                            const auto& key = token.key;
                            if (processed_input && key.bKeyDown && key_event_matches_ctrl_break(key))
                            {
                                // Mirror the inbox host: Ctrl+Break flushes the input buffer, generates a
                                // CTRL_BREAK_EVENT, and terminates raw reads with STATUS_ALERTED.
                                if (auto flushed = host_io.flush_input_buffer(); !flushed)
                                {
                                    return std::unexpected(flushed.error());
                                }

                                handle->decoded_input_pending.reset();
                                pending_prefix.clear();
                                handle->cooked_read_pending.clear();
                                handle->cooked_line_in_progress.clear();

                                state.for_each_process([&](const ProcessState& process) noexcept {
                                    (void)host_io.send_end_task(
                                        process.pid,
                                        CTRL_BREAK_EVENT,
                                        static_cast<DWORD>(core::console_ctrl_break_flag));
                                });

                                body.NumBytes = 0;
                                message.set_reply_status(core::status_alerted);
                                message.set_reply_information(0);
                                return outcome;
                            }
                            if (processed_input && key.bKeyDown && key_event_matches_ctrl_c(key))
                            {
                                state.for_each_process([&](const ProcessState& process) noexcept {
                                    (void)host_io.send_end_task(
                                        process.pid,
                                        CTRL_C_EVENT,
                                        static_cast<DWORD>(core::console_ctrl_c_flag));
                                });
                                continue;
                            }

                            if (!key.bKeyDown)
                            {
                                continue;
                            }

                            const wchar_t value = key.uChar.UnicodeChar;
                            if (value == L'\0')
                            {
                                continue;
                            }

                            const size_t repeat = std::max<size_t>(1, static_cast<size_t>(key.wRepeatCount));
                            const size_t to_write = std::min(repeat, max_wchars - written_units);
                            for (size_t i = 0; i < to_write; ++i)
                            {
                                out_chars[written_units + i] = value;
                            }
                            written_units += to_write;
                            continue;
                        }

                        const auto& chunk = token.text;
                        if (chunk.bytes_consumed == 0 || chunk.char_count == 0)
                        {
                            break;
                        }

                        if (processed_input && chunk.char_count == 1 && chunk.chars[0] == static_cast<wchar_t>(0x0003))
                        {
                            state.for_each_process([&](const ProcessState& process) noexcept {
                                (void)host_io.send_end_task(
                                    process.pid,
                                    CTRL_C_EVENT,
                                    static_cast<DWORD>(core::console_ctrl_c_flag));
                            });
                            continue;
                        }

                        if (split_surrogate)
                        {
                            out_chars[written_units] = chunk.chars[0];
                            handle->decoded_input_pending = chunk.chars[1];
                            ++written_units;
                            break;
                        }

                        for (size_t i = 0; i < chunk.char_count; ++i)
                        {
                            out_chars[written_units + i] = chunk.chars[i];
                        }
                        written_units += chunk.char_count;

                        if (host_io.input_bytes_available() == 0 && pending_prefix.empty())
                        {
                            break;
                        }
                    }

                    const size_t bytes_out = written_units * sizeof(wchar_t);
                    body.NumBytes = bytes_out > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                        ? std::numeric_limits<ULONG>::max()
                        : static_cast<ULONG>(bytes_out);
                }
                else
                {
                    // Raw ANSI reads are byte-oriented. We preserve the legacy behavior for non-VT bytes,
                    // but VT input sequences (win32-input-mode, DA1/focus responses, basic cursor keys)
                    // are consumed and never leak to the client as literal escape bytes.
                    //
                    // For win32-input-mode character keys, we encode the UnicodeChar as the configured
                    // input code page. Non-character key events (arrows, function keys) are consumed and
                    // ignored, matching the inbox host's "ReadConsole returns characters" contract.

                    const UINT code_page = static_cast<UINT>(state.input_code_page());
                    size_t bytes_written = 0;

                    const auto consume_from_stream = [&](const size_t byte_count) noexcept
                        -> std::expected<void, DeviceCommError> {
                        if (byte_count == 0)
                        {
                            return {};
                        }

                        const size_t pending_before = pending_prefix.size();
                        const size_t pending_consumed = std::min(byte_count, pending_before);
                        pending_prefix.consume_prefix(pending_consumed);

                        size_t remaining_to_discard = byte_count - pending_consumed;
                        std::array<std::byte, 16> discard{};
                        while (remaining_to_discard != 0)
                        {
                            const size_t discard_count = std::min(remaining_to_discard, discard.size());
                            auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), discard_count));
                            if (!removed)
                            {
                                return std::unexpected(removed.error());
                            }

                            if (removed.value() == 0)
                            {
                                break;
                            }

                            remaining_to_discard -= removed.value();
                        }

                        return {};
                    };

                    auto forward_ctrl_c = [&]() noexcept {
                        state.for_each_process([&](const ProcessState& process) noexcept {
                            (void)host_io.send_end_task(
                                process.pid,
                                CTRL_C_EVENT,
                                static_cast<DWORD>(core::console_ctrl_c_flag));
                        });
                    };

                    std::array<std::byte, 64> head{};
                    std::array<char, 16> encoded{};

                    for (;;)
                    {
                        if (bytes_written >= output->size())
                        {
                            break;
                        }

                        if (host_io.input_bytes_available() == 0 && pending_prefix.empty())
                        {
                            break;
                        }

                        const size_t pending_before = pending_prefix.size();
                        OC_ASSERT(pending_before <= head.size());
                        if (pending_before != 0)
                        {
                            const auto prefix = pending_prefix.bytes();
                            std::memcpy(head.data(), prefix.data(), pending_before);
                        }

                        size_t peeked_bytes = 0;
                        if (pending_before < head.size())
                        {
                            const size_t available = host_io.input_bytes_available();
                            const size_t to_peek = std::min(available, head.size() - pending_before);
                            if (to_peek != 0)
                            {
                                auto peeked = host_io.peek_input_bytes(std::span<std::byte>(head.data() + pending_before, to_peek));
                                if (!peeked)
                                {
                                    return std::unexpected(peeked.error());
                                }
                                peeked_bytes = peeked.value();
                            }
                        }

                        const size_t total_bytes = pending_before + peeked_bytes;
                        if (total_bytes == 0)
                        {
                            break;
                        }

                        vt_input::DecodedToken token{};
                        auto vt_outcome = vt_input::try_decode_vt(std::span<const std::byte>(head.data(), total_bytes), token);
                        if (vt_outcome == vt_input::DecodeResult::need_more_data && total_bytes == head.size())
                        {
                            // The token exceeds our supported buffering; fall back to raw byte consumption to
                            // avoid leaving the input stream in a permanently pending state.
                            vt_outcome = vt_input::DecodeResult::no_match;
                        }

                        if (vt_outcome == vt_input::DecodeResult::need_more_data)
                        {
                            if (bytes_written != 0)
                            {
                                break;
                            }

                            if (peeked_bytes != 0)
                            {
                                const auto drained = std::span<const std::byte>(head.data() + pending_before, peeked_bytes);
                                if (pending_prefix.append(drained))
                                {
                                    size_t remaining_to_discard = peeked_bytes;
                                    std::array<std::byte, 16> drain_discard{};
                                    while (remaining_to_discard != 0)
                                    {
                                        const size_t discard_count = std::min(remaining_to_discard, drain_discard.size());
                                        auto removed = host_io.read_input_bytes(std::span<std::byte>(drain_discard.data(), discard_count));
                                        if (!removed)
                                        {
                                            return std::unexpected(removed.error());
                                        }

                                        if (removed.value() == 0)
                                        {
                                            break;
                                        }

                                        remaining_to_discard -= removed.value();
                                    }
                                }
                            }

                            if (host_io.input_disconnected())
                            {
                                message.set_reply_status(core::status_unsuccessful);
                                message.set_reply_information(0);
                                return outcome;
                            }

                            outcome.reply_pending = true;
                            return outcome;
                        }

                        if (vt_outcome == vt_input::DecodeResult::produced)
                        {
                            if (token.bytes_consumed == 0)
                            {
                                break;
                            }

                            if (token.kind == vt_input::TokenKind::ignored_sequence)
                            {
                                if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                                {
                                    return std::unexpected(consumed.error());
                                }
                                continue;
                            }

                            if (token.kind == vt_input::TokenKind::key_event)
                            {
                                const auto& key = token.key;
                                if (processed_input && key.bKeyDown && key_event_matches_ctrl_break(key))
                                {
                                    if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                                    {
                                        return std::unexpected(consumed.error());
                                    }

                                    // Mirror the inbox host: Ctrl+Break flushes the input buffer and terminates
                                    // raw reads with STATUS_ALERTED.
                                    if (auto flushed = host_io.flush_input_buffer(); !flushed)
                                    {
                                        return std::unexpected(flushed.error());
                                    }

                                    handle->decoded_input_pending.reset();
                                    pending_prefix.clear();
                                    handle->cooked_read_pending.clear();
                                    handle->cooked_line_in_progress.clear();

                                    state.for_each_process([&](const ProcessState& process) noexcept {
                                        (void)host_io.send_end_task(
                                            process.pid,
                                            CTRL_BREAK_EVENT,
                                            static_cast<DWORD>(core::console_ctrl_break_flag));
                                    });

                                    body.NumBytes = 0;
                                    message.set_reply_status(core::status_alerted);
                                    message.set_reply_information(0);
                                    return outcome;
                                }
                                if (processed_input && key.bKeyDown && key_event_matches_ctrl_c(key))
                                {
                                    if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                                    {
                                        return std::unexpected(consumed.error());
                                    }
                                    forward_ctrl_c();
                                    continue;
                                }

                                if (!key.bKeyDown)
                                {
                                    if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                                    {
                                        return std::unexpected(consumed.error());
                                    }
                                    continue;
                                }

                                const wchar_t value = key.uChar.UnicodeChar;
                                if (value == L'\0')
                                {
                                    if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                                    {
                                        return std::unexpected(consumed.error());
                                    }
                                    continue;
                                }

                                const size_t remaining = output->size() - bytes_written;
                                const wchar_t src = value;
                                const int required = ::WideCharToMultiByte(
                                    code_page,
                                    0,
                                    &src,
                                    1,
                                    nullptr,
                                    0,
                                    nullptr,
                                    nullptr);
                                if (required <= 0)
                                {
                                    message.set_reply_status(core::status_invalid_parameter);
                                    message.set_reply_information(0);
                                    return outcome;
                                }

                                if (static_cast<size_t>(required) > remaining)
                                {
                                    // Not enough space: preserve the VT sequence for the next read.
                                    break;
                                }

                                if (static_cast<size_t>(required) > encoded.size())
                                {
                                    message.set_reply_status(core::status_invalid_parameter);
                                    message.set_reply_information(0);
                                    return outcome;
                                }

                                const int converted = ::WideCharToMultiByte(
                                    code_page,
                                    0,
                                    &src,
                                    1,
                                    encoded.data(),
                                    required,
                                    nullptr,
                                    nullptr);
                                if (converted != required)
                                {
                                    message.set_reply_status(core::status_invalid_parameter);
                                    message.set_reply_information(0);
                                    return outcome;
                                }

                                for (int i = 0; i < required; ++i)
                                {
                                    (*output)[bytes_written + static_cast<size_t>(i)] = static_cast<std::byte>(encoded[static_cast<size_t>(i)]);
                                }
                                bytes_written += static_cast<size_t>(required);

                                if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                                {
                                    return std::unexpected(consumed.error());
                                }
                                continue;
                            }

                            if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                            {
                                return std::unexpected(consumed.error());
                            }
                            continue;
                        }

                        // No VT match: preserve the legacy raw-byte behavior (except Ctrl+C filtering in processed mode).
                        const std::byte value = head[0];
                        if (processed_input && value == static_cast<std::byte>(0x03))
                        {
                            if (auto consumed = consume_from_stream(1); !consumed)
                            {
                                return std::unexpected(consumed.error());
                            }
                            forward_ctrl_c();
                            continue;
                        }

                        (*output)[bytes_written] = value;
                        ++bytes_written;

                        if (auto consumed = consume_from_stream(1); !consumed)
                        {
                            return std::unexpected(consumed.error());
                        }
                    }

                    if (bytes_written == 0 && host_io.input_bytes_available() == 0 && pending_prefix.empty())
                    {
                        if (host_io.input_disconnected())
                        {
                            message.set_reply_status(core::status_unsuccessful);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        outcome.reply_pending = true;
                        return outcome;
                    }

                    body.NumBytes = bytes_written > static_cast<size_t>(std::numeric_limits<ULONG>::max())
                        ? std::numeric_limits<ULONG>::max()
                        : static_cast<ULONG>(bytes_written);
                }

                message.set_reply_status(core::status_success);
                message.set_reply_information(body.NumBytes);
                return outcome;
            }

            reject_user_defined_not_implemented();
            return outcome;
        }
        case console_io_connect:
        {
            const auto pid64 = static_cast<unsigned long long>(descriptor.process);
            const auto tid64 = static_cast<unsigned long long>(descriptor.object);
            const DWORD pid = pid64 > 0xFFFF'FFFFULL ? 0 : static_cast<DWORD>(pid64);
            const DWORD tid = tid64 > 0xFFFF'FFFFULL ? 0 : static_cast<DWORD>(tid64);

            // CONNECT input contains a `CONSOLE_SERVER_MSG` payload. We only use the application name
            // for command history allocation; other fields are currently ignored.
            std::wstring_view app_name{};
            if (auto input = message.get_input_buffer())
            {
                if (input->size() >= sizeof(CONSOLE_SERVER_MSG))
                {
                    CONSOLE_SERVER_MSG data{};
                    std::memcpy(&data, input->data(), sizeof(data));

                    const size_t bytes = static_cast<size_t>(data.ApplicationNameLength);
                    const bool aligned = (bytes % sizeof(wchar_t)) == 0;
                    const bool within_buffer = bytes <= (sizeof(data.ApplicationName) - sizeof(wchar_t));
                    const size_t cch = aligned ? (bytes / sizeof(wchar_t)) : 0;
                    const bool has_terminator = aligned && cch < std::size(data.ApplicationName) && data.ApplicationName[cch] == L'\0';

                    if (aligned && within_buffer && has_terminator)
                    {
                        app_name = std::wstring_view(data.ApplicationName, cch);
                    }
                }
            }
            else
            {
                return std::unexpected(input.error());
            }

            auto info = state.connect_client(pid, tid, app_name);
            if (!info)
            {
                message.set_reply_status(core::status_no_memory);
                message.set_reply_information(0);
                return outcome;
            }

            message.set_reply_status(core::status_success);
            message.set_reply_information(sizeof(ConnectionInformation));
            message.set_completion_write_data(info.value());
            return outcome;
        }
        case console_io_disconnect:
        {
            const ULONG_PTR process_handle = descriptor.process;
            const bool removed = state.disconnect_client(process_handle);

            message.set_reply_status(removed ? core::status_success : core::status_invalid_handle);
            message.set_reply_information(0);

            if (state.process_count() == 0)
            {
                outcome.request_exit = true;
            }
            return outcome;
        }
        case console_io_create_object:
        {
            auto create_info = message.packet().payload.create_object.create_object;

            if (create_info.object_type == io_object_type_generic)
            {
                const auto access = create_info.desired_access & (GENERIC_READ | GENERIC_WRITE);
                if (access == GENERIC_READ)
                {
                    create_info.object_type = io_object_type_current_input;
                }
                else if (access == GENERIC_WRITE)
                {
                    create_info.object_type = io_object_type_current_output;
                }
            }

            ObjectHandle object{};
            object.desired_access = create_info.desired_access;
            object.share_mode = create_info.share_mode;
            object.owning_process = descriptor.process;

            switch (create_info.object_type)
            {
            case io_object_type_current_input:
                object.kind = ObjectKind::input;
                break;
            case io_object_type_current_output:
                object.kind = ObjectKind::output;
                object.screen_buffer = state.active_screen_buffer();
                break;
            case io_object_type_new_output:
                object.kind = ObjectKind::output;
                if (auto created = state.create_screen_buffer_like_active(); created)
                {
                    object.screen_buffer = std::move(created.value());
                }
                else
                {
                    message.set_reply_status(core::status_no_memory);
                    message.set_reply_information(0);
                    return outcome;
                }
                break;
            default:
                message.set_reply_status(core::status_invalid_parameter);
                message.set_reply_information(0);
                return outcome;
            }

            if (!state.has_process(descriptor.process))
            {
                message.set_reply_status(core::status_invalid_handle);
                message.set_reply_information(0);
                return outcome;
            }

            auto handle_id = state.create_object(object);
            if (!handle_id)
            {
                message.set_reply_status(core::status_no_memory);
                message.set_reply_information(0);
                return outcome;
            }

            message.set_reply_status(core::status_success);
            message.set_reply_information(handle_id.value());
            return outcome;
        }
        case console_io_close_object:
        {
            const ULONG_PTR handle_id = descriptor.object;
            const bool closed = state.close_object(handle_id);

            message.set_reply_status(closed ? core::status_success : core::status_invalid_handle);
            message.set_reply_information(0);
            return outcome;
        }
        case console_io_raw_flush:
        {
            const auto handle_id = descriptor.object;
            auto* handle = state.find_object(handle_id);
            if (handle == nullptr || handle->kind != ObjectKind::input)
            {
                message.set_reply_status(core::status_invalid_handle);
                message.set_reply_information(0);
                return outcome;
            }

            if (auto flushed = host_io.flush_input_buffer(); !flushed)
            {
                return std::unexpected(flushed.error());
            }

            handle->decoded_input_pending.reset();
            handle->pending_input_bytes.clear();
            handle->cooked_read_pending.clear();
            handle->cooked_line_in_progress.clear();
            handle->cooked_line_cursor = 0;
            handle->cooked_insert_mode = true;

            message.set_reply_status(core::status_success);
            message.set_reply_information(0);
            return outcome;
        }
        case console_io_raw_write:
        {
            const auto handle_id = descriptor.object;
            auto* handle = state.find_object(handle_id);
            if (handle == nullptr || handle->kind != ObjectKind::output)
            {
                message.set_reply_status(core::status_invalid_handle);
                message.set_reply_information(0);
                return outcome;
            }

            auto* screen_buffer = handle->screen_buffer.get();
            if (screen_buffer == nullptr)
            {
                message.set_reply_status(core::status_invalid_handle);
                message.set_reply_information(0);
                return outcome;
            }

            auto input = message.get_input_buffer();
            if (!input)
            {
                return std::unexpected(input.error());
            }

            std::wstring decoded_text;
            {
                const UINT code_page = static_cast<UINT>(state.output_code_page());
                auto decoded = decode_console_string(
                    false,
                    std::span<const std::byte>(input->data(), input->size()),
                    code_page,
                    L"RAW_WRITE decode failed");
                if (!decoded)
                {
                    message.set_reply_status(decoded.error().win32_error == ERROR_OUTOFMEMORY ? core::status_no_memory : core::status_invalid_parameter);
                    message.set_reply_information(0);
                    return outcome;
                }
                decoded_text = std::move(decoded.value());
            }

            auto written = host_io.write_output_bytes(*input);
            if (!written)
            {
                return std::unexpected(written.error());
            }

            apply_text_to_screen_buffer(*screen_buffer, decoded_text, state.output_mode(), &state, &host_io);

            message.set_reply_status(core::status_success);
            message.set_reply_information(static_cast<ULONG_PTR>(written.value()));
            return outcome;
        }
        case console_io_raw_read:
        {
            const auto handle_id = descriptor.object;
            auto* handle = state.find_object(handle_id);
            if (handle == nullptr || handle->kind != ObjectKind::input)
            {
                message.set_reply_status(core::status_invalid_handle);
                message.set_reply_information(0);
                return outcome;
            }

            auto output = message.get_output_buffer();
            if (!output)
            {
                return std::unexpected(output.error());
            }

            const bool processed_input = (state.input_mode() & ENABLE_PROCESSED_INPUT) != 0;
            size_t bytes_written = 0;
            auto& pending_prefix = handle->pending_input_bytes;

            const UINT code_page = static_cast<UINT>(state.input_code_page());

            const auto forward_ctrl_c = [&]() noexcept {
                state.for_each_process([&](const ProcessState& process) noexcept {
                    (void)host_io.send_end_task(
                        process.pid,
                        CTRL_C_EVENT,
                        static_cast<DWORD>(core::console_ctrl_c_flag));
                });
            };

            const auto forward_ctrl_break = [&]() noexcept {
                state.for_each_process([&](const ProcessState& process) noexcept {
                    (void)host_io.send_end_task(
                        process.pid,
                        CTRL_BREAK_EVENT,
                        static_cast<DWORD>(core::console_ctrl_break_flag));
                });
            };

            const auto consume_from_stream = [&](const size_t count) noexcept -> std::expected<void, DeviceCommError> {
                size_t pending_consumed = std::min(count, pending_prefix.size());
                pending_prefix.consume_prefix(pending_consumed);

                size_t remaining = count - pending_consumed;
                std::array<std::byte, 16> discard{};
                while (remaining != 0)
                {
                    const size_t to_discard = std::min(remaining, discard.size());
                    auto removed = host_io.read_input_bytes(std::span<std::byte>(discard.data(), to_discard));
                    if (!removed)
                    {
                        return std::unexpected(removed.error());
                    }

                    const size_t removed_bytes = removed.value();
                    if (removed_bytes == 0)
                    {
                        break;
                    }

                    remaining -= removed_bytes;
                }

                return {};
            };

            std::array<std::byte, 64> head{};
            std::array<char, 16> encoded{};

            for (;;)
            {
                if (bytes_written >= output->size())
                {
                    break;
                }

                if (host_io.input_bytes_available() == 0 && pending_prefix.empty())
                {
                    break;
                }

                const size_t pending_before = pending_prefix.size();
                OC_ASSERT(pending_before <= head.size());
                if (pending_before != 0)
                {
                    const auto prefix = pending_prefix.bytes();
                    std::memcpy(head.data(), prefix.data(), pending_before);
                }

                size_t peeked_bytes = 0;
                if (pending_before < head.size())
                {
                    const size_t available = host_io.input_bytes_available();
                    const size_t to_peek = std::min(available, head.size() - pending_before);
                    if (to_peek != 0)
                    {
                        auto peeked = host_io.peek_input_bytes(std::span<std::byte>(head.data() + pending_before, to_peek));
                        if (!peeked)
                        {
                            return std::unexpected(peeked.error());
                        }
                        peeked_bytes = peeked.value();
                    }
                }

                const size_t total_bytes = pending_before + peeked_bytes;
                if (total_bytes == 0)
                {
                    break;
                }

                vt_input::DecodedToken token{};
                auto vt_outcome = vt_input::try_decode_vt(std::span<const std::byte>(head.data(), total_bytes), token);
                if (vt_outcome == vt_input::DecodeResult::need_more_data && total_bytes == head.size())
                {
                    // The token exceeds our supported buffering; fall back to raw byte consumption to
                    // avoid leaving the input stream in a permanently pending state.
                    vt_outcome = vt_input::DecodeResult::no_match;
                }

                if (vt_outcome == vt_input::DecodeResult::need_more_data)
                {
                    if (bytes_written != 0)
                    {
                        break;
                    }

                    if (peeked_bytes != 0)
                    {
                        const auto drained = std::span<const std::byte>(head.data() + pending_before, peeked_bytes);
                        if (pending_prefix.append(drained))
                        {
                            size_t remaining_to_discard = peeked_bytes;
                            std::array<std::byte, 16> drain_discard{};
                            while (remaining_to_discard != 0)
                            {
                                const size_t discard_count = std::min(remaining_to_discard, drain_discard.size());
                                auto removed = host_io.read_input_bytes(std::span<std::byte>(drain_discard.data(), discard_count));
                                if (!removed)
                                {
                                    return std::unexpected(removed.error());
                                }

                                if (removed.value() == 0)
                                {
                                    break;
                                }

                                remaining_to_discard -= removed.value();
                            }
                        }
                    }

                    if (host_io.input_disconnected())
                    {
                        message.set_reply_status(core::status_unsuccessful);
                        message.set_reply_information(0);
                        return outcome;
                    }

                    outcome.reply_pending = true;
                    return outcome;
                }

                if (vt_outcome == vt_input::DecodeResult::produced)
                {
                    if (token.bytes_consumed == 0)
                    {
                        break;
                    }

                    if (token.kind == vt_input::TokenKind::ignored_sequence)
                    {
                        if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                        {
                            return std::unexpected(consumed.error());
                        }
                        continue;
                    }

                    if (token.kind == vt_input::TokenKind::key_event)
                    {
                        const auto& key = token.key;
                        if (processed_input && key.bKeyDown && key_event_matches_ctrl_break(key))
                        {
                            if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                            {
                                return std::unexpected(consumed.error());
                            }

                            if (auto flushed = host_io.flush_input_buffer(); !flushed)
                            {
                                return std::unexpected(flushed.error());
                            }

                            handle->decoded_input_pending.reset();
                            pending_prefix.clear();
                            handle->cooked_read_pending.clear();
                            handle->cooked_line_in_progress.clear();

                            forward_ctrl_break();

                            message.set_reply_status(core::status_alerted);
                            message.set_reply_information(0);
                            return outcome;
                        }
                        if (processed_input && key.bKeyDown && key_event_matches_ctrl_c(key))
                        {
                            if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                            {
                                return std::unexpected(consumed.error());
                            }
                            forward_ctrl_c();
                            continue;
                        }

                        if (!key.bKeyDown)
                        {
                            if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                            {
                                return std::unexpected(consumed.error());
                            }
                            continue;
                        }

                        const wchar_t value = key.uChar.UnicodeChar;
                        if (value == L'\0')
                        {
                            if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                            {
                                return std::unexpected(consumed.error());
                            }
                            continue;
                        }

                        if (bytes_written == 0 && value == static_cast<wchar_t>(0x001A))
                        {
                            // Match the inbox host's `ProcessControlZ` behavior used by raw reads:
                            // CTRL+Z at the start of the read is treated as EOF (0 bytes).
                            if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                            {
                                return std::unexpected(consumed.error());
                            }

                            if (!output->empty())
                            {
                                output->front() = std::byte{ 0 };
                            }

                            message.set_reply_status(core::status_success);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        const size_t remaining = output->size() - bytes_written;
                        const wchar_t src = value;
                        const int required = ::WideCharToMultiByte(
                            code_page,
                            0,
                            &src,
                            1,
                            nullptr,
                            0,
                            nullptr,
                            nullptr);
                        if (required <= 0)
                        {
                            message.set_reply_status(core::status_invalid_parameter);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        if (static_cast<size_t>(required) > remaining)
                        {
                            // Not enough space: preserve the VT sequence for the next read.
                            break;
                        }

                        if (static_cast<size_t>(required) > encoded.size())
                        {
                            message.set_reply_status(core::status_invalid_parameter);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        const int converted = ::WideCharToMultiByte(
                            code_page,
                            0,
                            &src,
                            1,
                            encoded.data(),
                            required,
                            nullptr,
                            nullptr);
                        if (converted != required)
                        {
                            message.set_reply_status(core::status_invalid_parameter);
                            message.set_reply_information(0);
                            return outcome;
                        }

                        for (int i = 0; i < required; ++i)
                        {
                            (*output)[bytes_written + static_cast<size_t>(i)] = static_cast<std::byte>(encoded[static_cast<size_t>(i)]);
                        }
                        bytes_written += static_cast<size_t>(required);

                        if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                        {
                            return std::unexpected(consumed.error());
                        }
                        continue;
                    }

                    if (auto consumed = consume_from_stream(token.bytes_consumed); !consumed)
                    {
                        return std::unexpected(consumed.error());
                    }
                    continue;
                }

                // No VT match: preserve the legacy raw-byte behavior (except Ctrl+C filtering in processed mode).
                const std::byte value = head[0];
                if (processed_input && value == static_cast<std::byte>(0x03))
                {
                    if (auto consumed = consume_from_stream(1); !consumed)
                    {
                        return std::unexpected(consumed.error());
                    }
                    forward_ctrl_c();
                    continue;
                }

                if (bytes_written == 0 && value == static_cast<std::byte>(0x1a))
                {
                    // Match the inbox host's `ProcessControlZ` behavior used by raw reads:
                    // CTRL+Z at the start of the read is treated as EOF (0 bytes).
                    if (auto consumed = consume_from_stream(1); !consumed)
                    {
                        return std::unexpected(consumed.error());
                    }

                    if (!output->empty())
                    {
                        output->front() = std::byte{ 0 };
                    }

                    message.set_reply_status(core::status_success);
                    message.set_reply_information(0);
                    return outcome;
                }

                (*output)[bytes_written] = value;
                ++bytes_written;

                if (auto consumed = consume_from_stream(1); !consumed)
                {
                    return std::unexpected(consumed.error());
                }
            }

            if (bytes_written == 0 && host_io.input_bytes_available() == 0 && pending_prefix.empty())
            {
                if (host_io.input_disconnected())
                {
                    message.set_reply_status(core::status_unsuccessful);
                    message.set_reply_information(0);
                    return outcome;
                }

                outcome.reply_pending = true;
                return outcome;
            }

            message.set_reply_status(core::status_success);
            message.set_reply_information(static_cast<ULONG_PTR>(bytes_written));
            return outcome;
        }
        default:
            message.set_reply_status(core::status_not_implemented);
            message.set_reply_information(0);
            return outcome;
        }
    }

    class ConDrvServer final
    {
    public:
        [[nodiscard]] static std::expected<DWORD, ServerError> run(
            core::HandleView server_handle,
            core::HandleView signal_handle,
            core::HandleView host_input,
            core::HandleView host_output,
            core::HandleView host_signal_pipe,
            logging::Logger& logger) noexcept;

        // Windowed host entry point: publishes `ScreenBuffer` viewport snapshots to the UI thread.
        // `paint_target` is the HWND that will receive `WM_APP + 1` invalidation messages.
        [[nodiscard]] static std::expected<DWORD, ServerError> run(
            core::HandleView server_handle,
            core::HandleView signal_handle,
            core::HandleView host_input,
            core::HandleView host_output,
            core::HandleView host_signal_pipe,
            logging::Logger& logger,
            std::shared_ptr<PublishedScreenBuffer> published,
            HWND paint_target) noexcept;

        // Handoff entry point used by `-Embedding` scenarios: a pending IO
        // descriptor is provided by the inbox host via a portable attach
        // message. We must complete it using the same server state that will
        // subsequently service new IOs.
        [[nodiscard]] static std::expected<DWORD, ServerError> run_with_handoff(
            core::HandleView server_handle,
            core::HandleView signal_handle,
            core::HandleView input_available_event,
            core::HandleView host_input,
            core::HandleView host_output,
            core::HandleView host_signal_pipe,
            const IoPacket& initial_packet,
            logging::Logger& logger) noexcept;

        // Windowed variant of the handoff entry point. This is used when the
        // inbox host already consumed the first `IOCTL_CONDRV_READ_IO` packet
        // (to probe default-terminal delegation) but must still fall back to a
        // classic windowed host when delegation fails.
        [[nodiscard]] static std::expected<DWORD, ServerError> run_with_handoff(
            core::HandleView server_handle,
            core::HandleView signal_handle,
            core::HandleView input_available_event,
            core::HandleView host_input,
            core::HandleView host_output,
            core::HandleView host_signal_pipe,
            const IoPacket& initial_packet,
            logging::Logger& logger,
            std::shared_ptr<PublishedScreenBuffer> published,
            HWND paint_target) noexcept;
    };
}
