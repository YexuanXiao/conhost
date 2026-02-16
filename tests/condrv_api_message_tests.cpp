#include "condrv/condrv_api_message.hpp"

#include <cstddef>
#include <cstring>
#include <vector>

namespace
{
    struct FakeComm final
    {
        int read_calls{ 0 };
        int write_calls{ 0 };
        int complete_calls{ 0 };

        oc::condrv::IoOperation last_write{};
        std::vector<std::byte> written_bytes{};
        oc::condrv::IoComplete last_complete{};

        std::expected<void, oc::condrv::DeviceCommError> read_input(oc::condrv::IoOperation& op) noexcept
        {
            ++read_calls;

            if (op.buffer.data == nullptr)
            {
                return std::unexpected(oc::condrv::DeviceCommError{
                    .context = L"Fake read_input received null buffer",
                    .win32_error = ERROR_INVALID_PARAMETER,
                });
            }

            auto* bytes = static_cast<std::byte*>(op.buffer.data);
            for (ULONG i = 0; i < op.buffer.size; ++i)
            {
                bytes[i] = static_cast<std::byte>((op.buffer.offset + i) & 0xFF);
            }

            return {};
        }

        std::expected<void, oc::condrv::DeviceCommError> write_output(oc::condrv::IoOperation& op) noexcept
        {
            ++write_calls;
            last_write = op;

            if (op.buffer.data == nullptr)
            {
                return std::unexpected(oc::condrv::DeviceCommError{
                    .context = L"Fake write_output received null buffer",
                    .win32_error = ERROR_INVALID_PARAMETER,
                });
            }

            const auto* bytes = static_cast<const std::byte*>(op.buffer.data);
            written_bytes.assign(bytes, bytes + op.buffer.size);
            return {};
        }

        std::expected<void, oc::condrv::DeviceCommError> complete_io(const oc::condrv::IoComplete& complete) noexcept
        {
            ++complete_calls;
            last_complete = complete;
            return {};
        }
    };

    [[nodiscard]] oc::condrv::IoPacket make_packet(const ULONG input_size, const ULONG output_size) noexcept
    {
        oc::condrv::IoPacket packet{};
        packet.descriptor.identifier.LowPart = 1;
        packet.descriptor.identifier.HighPart = 0;
        packet.descriptor.function = oc::condrv::console_io_user_defined;
        packet.descriptor.input_size = input_size;
        packet.descriptor.output_size = output_size;
        return packet;
    }

    bool test_input_buffer_reads_once()
    {
        FakeComm comm{};

        auto packet = make_packet(8, 0);
        oc::condrv::BasicApiMessage<FakeComm> message(comm, packet);

        const auto input1 = message.get_input_buffer();
        if (!input1 || input1->size() != 8)
        {
            return false;
        }

        const auto input2 = message.get_input_buffer();
        if (!input2 || input2->data() != input1->data())
        {
            return false;
        }

        if (comm.read_calls != 1)
        {
            return false;
        }

        for (size_t i = 0; i < input1->size(); ++i)
        {
            if ((*input1)[i] != static_cast<std::byte>(i))
            {
                return false;
            }
        }

        return true;
    }

    bool test_output_buffer_writes_on_release()
    {
        FakeComm comm{};

        auto packet = make_packet(0, 6);
        oc::condrv::BasicApiMessage<FakeComm> message(comm, packet);

        auto output = message.get_output_buffer();
        if (!output || output->size() != 6)
        {
            return false;
        }

        for (size_t i = 0; i < output->size(); ++i)
        {
            (*output)[i] = static_cast<std::byte>(0xA0 + i);
        }

        message.set_reply_status(static_cast<NTSTATUS>(0));
        message.set_reply_information(6);

        const auto released = message.release_message_buffers();
        if (!released)
        {
            return false;
        }

        if (comm.write_calls != 1)
        {
            return false;
        }

        if (comm.written_bytes.size() != 6)
        {
            return false;
        }

        for (size_t i = 0; i < comm.written_bytes.size(); ++i)
        {
            if (comm.written_bytes[i] != static_cast<std::byte>(0xA0 + i))
            {
                return false;
            }
        }

        return true;
    }

    bool test_release_skips_write_on_failure_status()
    {
        FakeComm comm{};

        auto packet = make_packet(0, 4);
        oc::condrv::BasicApiMessage<FakeComm> message(comm, packet);

        auto output = message.get_output_buffer();
        if (!output)
        {
            return false;
        }

        message.set_reply_status(static_cast<NTSTATUS>(0xC0000001L));
        message.set_reply_information(4);

        const auto released = message.release_message_buffers();
        if (!released)
        {
            return false;
        }

        return comm.write_calls == 0;
    }

    bool test_invalid_offsets_fail()
    {
        FakeComm comm{};

        auto packet = make_packet(2, 0);
        oc::condrv::BasicApiMessage<FakeComm> message(comm, packet);
        message.set_read_offset(3);

        const auto result = message.get_input_buffer();
        return !result.has_value();
    }

    bool test_complete_io_forwards_completion()
    {
        FakeComm comm{};

        auto packet = make_packet(0, 0);
        oc::condrv::BasicApiMessage<FakeComm> message(comm, packet);
        message.set_reply_status(static_cast<NTSTATUS>(0));
        message.set_reply_information(0);

        const auto completed = message.complete_io();
        if (!completed)
        {
            return false;
        }

        if (comm.complete_calls != 1)
        {
            return false;
        }

        return comm.last_complete.identifier.LowPart == 1;
    }

    bool test_completion_write_data_copies_payload()
    {
        FakeComm comm{};

        auto packet = make_packet(0, 0);
        oc::condrv::BasicApiMessage<FakeComm> message(comm, packet);

        oc::condrv::ConnectionInformation info{};
        info.process = 0x1111;
        info.input = 0x2222;
        info.output = 0x3333;

        message.set_reply_status(static_cast<NTSTATUS>(0));
        message.set_reply_information(sizeof(info));
        message.set_completion_write_data(info);

        const auto completed = message.complete_io();
        if (!completed)
        {
            return false;
        }

        if (comm.last_complete.write.data == nullptr)
        {
            return false;
        }

        if (comm.last_complete.write.size != sizeof(info))
        {
            return false;
        }

        oc::condrv::ConnectionInformation round_trip{};
        std::memcpy(&round_trip, comm.last_complete.write.data, sizeof(round_trip));

        return round_trip.process == info.process &&
               round_trip.input == info.input &&
               round_trip.output == info.output;
    }
}

bool run_condrv_api_message_tests()
{
    return test_input_buffer_reads_once() &&
           test_output_buffer_writes_on_release() &&
           test_release_skips_write_on_failure_status() &&
           test_invalid_offsets_fail() &&
           test_complete_io_forwards_completion() &&
           test_completion_write_data_copies_payload();
}
