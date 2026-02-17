#include "logging/logger.hpp"

#include <Windows.h>

#include <memory>
#include <string>

namespace
{
    class TestSink final : public oc::logging::ILogSink
    {
    public:
        void write(const std::wstring_view line) noexcept override
        {
            captured = std::wstring{ line };
            ++writes;
        }

        std::wstring captured;
        int writes{ 0 };
    };

    bool test_level_filtering()
    {
        auto sink = std::make_shared<TestSink>();
        oc::logging::Logger logger(oc::logging::LogLevel::warning);
        logger.add_sink(sink);

        logger.log(oc::logging::LogLevel::info, L"this should be filtered");
        if (sink->writes != 0)
        {
            return false;
        }

        logger.log(oc::logging::LogLevel::error, L"{}", L"this should pass");
        return sink->writes == 1 && sink->captured.find(L"this should pass") != std::wstring::npos;
    }

    bool test_file_sink_create()
    {
        const auto sink = oc::logging::FileLogSink::create(L"logger_test.log");
        if (!sink)
        {
            return false;
        }

        sink.value()->write(L"logger file sink smoke");
        ::DeleteFileW(L"logger_test.log");
        return true;
    }

    bool test_default_file_sink_path()
    {
        const auto resolved = oc::logging::FileLogSink::resolve_default_log_path();
        if (!resolved || resolved->empty())
        {
            return false;
        }

        const size_t last_separator = resolved->find_last_of(L"\\/");
        if (last_separator == std::wstring::npos || last_separator + 1 >= resolved->size())
        {
            return false;
        }

        const std::wstring file_name = resolved->substr(last_separator + 1);
        const std::wstring expected_prefix = L"console_" + std::to_wstring(::GetCurrentProcessId()) + L"_";
        if (!file_name.starts_with(expected_prefix))
        {
            return false;
        }
        if (!file_name.ends_with(L".log"))
        {
            return false;
        }

        {
            const auto sink = oc::logging::FileLogSink::create(*resolved);
            if (!sink)
            {
                return false;
            }

            sink.value()->write(L"default file sink path smoke");
        }
        return ::DeleteFileW(resolved->c_str()) != FALSE;
    }
}

bool run_logger_tests()
{
    return test_level_filtering() &&
           test_file_sink_create() &&
           test_default_file_sink_path();
}
