#include "condrv/condrv_device_comm.hpp"
#include "condrv/condrv_protocol.hpp"

#include <type_traits>

namespace
{
    bool test_condrv_protocol_layout()
    {
        static_assert(std::is_standard_layout_v<oc::condrv::IoDescriptor>);
        static_assert(std::is_trivially_copyable_v<oc::condrv::IoDescriptor>);
        static_assert(sizeof(oc::condrv::IoDescriptor) >= sizeof(LUID));

        static_assert(std::is_standard_layout_v<oc::condrv::IoComplete>);
        static_assert(std::is_trivially_copyable_v<oc::condrv::IoComplete>);

        static_assert(std::is_standard_layout_v<oc::condrv::IoOperation>);
        static_assert(std::is_trivially_copyable_v<oc::condrv::IoOperation>);

        static_assert(oc::condrv::ioctl_read_io != 0);
        static_assert(oc::condrv::ioctl_complete_io != 0);
        static_assert(oc::condrv::ioctl_set_server_information != 0);
        return true;
    }

    bool test_device_comm_rejects_invalid_handle()
    {
        auto created = oc::condrv::ConDrvDeviceComm::from_server_handle(oc::core::HandleView(INVALID_HANDLE_VALUE));
        if (created.has_value())
        {
            return false;
        }

        return created.error().win32_error == ERROR_INVALID_HANDLE;
    }
}

bool run_condrv_protocol_tests()
{
    return test_condrv_protocol_layout() &&
           test_device_comm_rejects_invalid_handle();
}
