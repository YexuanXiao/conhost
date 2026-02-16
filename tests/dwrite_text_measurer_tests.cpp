#include "renderer/dwrite_text_measurer.hpp"

#include <Windows.h>

#include <cstdlib>
#include <cstdio>

namespace
{
    class CoInitScope final
    {
    public:
        explicit CoInitScope(const HRESULT hr) noexcept :
            _hr(hr)
        {
        }

        ~CoInitScope() noexcept
        {
            if (_hr == S_OK || _hr == S_FALSE)
            {
                ::CoUninitialize();
            }
        }

        CoInitScope(const CoInitScope&) = delete;
        CoInitScope& operator=(const CoInitScope&) = delete;

        [[nodiscard]] HRESULT result() const noexcept
        {
            return _hr;
        }

    private:
        HRESULT _hr{ E_FAIL };
    };

    [[nodiscard]] bool test_measure_consolas_basic()
    {
        auto measurer = oc::renderer::DwriteTextMeasurer::create();
        if (!measurer)
        {
            fwprintf(stderr, L"[DETAIL] DwriteTextMeasurer::create failed (err=%lu)\n", oc::core::to_dword(measurer.error()));
            return false;
        }

        oc::renderer::FontRequest request{};
        request.family_name = L"Consolas";
        request.size_points = 12.0f;
        request.dpi = 96.0f;

        const auto metrics = (*measurer)->measure_font(request);
        if (!metrics)
        {
            fwprintf(stderr, L"[DETAIL] measure_font failed (err=%lu)\n", oc::core::to_dword(metrics.error()));
            return false;
        }

        if (metrics->resolved_family_name.empty())
        {
            fwprintf(stderr, L"[DETAIL] resolved_family_name was empty\n");
            return false;
        }

        if (metrics->cell.width_px <= 0 || metrics->cell.height_px <= 0)
        {
            fwprintf(stderr, L"[DETAIL] cell metrics invalid (w=%d h=%d)\n", metrics->cell.width_px, metrics->cell.height_px);
            return false;
        }

        if (metrics->cell.baseline_px <= 0 || metrics->cell.baseline_px > metrics->cell.height_px)
        {
            fwprintf(stderr, L"[DETAIL] baseline out of range (baseline=%d height=%d)\n", metrics->cell.baseline_px, metrics->cell.height_px);
            return false;
        }

        return true;
    }

    [[nodiscard]] bool test_missing_font_falls_back_to_consolas()
    {
        auto measurer = oc::renderer::DwriteTextMeasurer::create();
        if (!measurer)
        {
            fwprintf(stderr, L"[DETAIL] DwriteTextMeasurer::create failed (err=%lu)\n", oc::core::to_dword(measurer.error()));
            return false;
        }

        oc::renderer::FontRequest request{};
        request.family_name = L"ThisFontShouldNotExist_OpenConsoleNew";
        request.size_points = 12.0f;
        request.dpi = 96.0f;

        const auto metrics = (*measurer)->measure_font(request);
        if (!metrics)
        {
            fwprintf(stderr, L"[DETAIL] measure_font failed (err=%lu)\n", oc::core::to_dword(metrics.error()));
            return false;
        }

        if (metrics->resolved_family_name != L"Consolas")
        {
            fwprintf(stderr, L"[DETAIL] expected fallback to Consolas, got '%ls'\n", metrics->resolved_family_name.c_str());
            return false;
        }

        return true;
    }

    [[nodiscard]] bool test_dpi_scaling_is_linear()
    {
        auto measurer = oc::renderer::DwriteTextMeasurer::create();
        if (!measurer)
        {
            fwprintf(stderr, L"[DETAIL] DwriteTextMeasurer::create failed (err=%lu)\n", oc::core::to_dword(measurer.error()));
            return false;
        }

        oc::renderer::FontRequest a{};
        a.family_name = L"Consolas";
        a.size_points = 12.0f;
        a.dpi = 96.0f;

        oc::renderer::FontRequest b = a;
        b.dpi = 192.0f;

        const auto m1 = (*measurer)->measure_font(a);
        const auto m2 = (*measurer)->measure_font(b);
        if (!m1 || !m2)
        {
            fwprintf(stderr, L"[DETAIL] measure_font failed (err=%lu/%lu)\n",
                m1 ? 0ul : oc::core::to_dword(m1.error()),
                m2 ? 0ul : oc::core::to_dword(m2.error()));
            return false;
        }

        const int w1 = m1->cell.width_px;
        const int h1 = m1->cell.height_px;
        const int w2 = m2->cell.width_px;
        const int h2 = m2->cell.height_px;

        // The algorithm is linear with respect to DPI, but the public values
        // are rounded to integer pixels. Allow +/-1 for rounding.
        const int expected_w2 = w1 * 2;
        const int expected_h2 = h1 * 2;
        if (std::abs(w2 - expected_w2) > 1 || std::abs(h2 - expected_h2) > 1)
        {
            fwprintf(stderr, L"[DETAIL] dpi scaling not ~2x (96dpi=%dx%d 192dpi=%dx%d)\n", w1, h1, w2, h2);
            return false;
        }

        return true;
    }

    [[nodiscard]] bool test_point_size_scaling_is_linear()
    {
        auto measurer = oc::renderer::DwriteTextMeasurer::create();
        if (!measurer)
        {
            fwprintf(stderr, L"[DETAIL] DwriteTextMeasurer::create failed (err=%lu)\n", oc::core::to_dword(measurer.error()));
            return false;
        }

        oc::renderer::FontRequest a{};
        a.family_name = L"Consolas";
        a.size_points = 12.0f;
        a.dpi = 96.0f;

        oc::renderer::FontRequest b = a;
        b.size_points = 24.0f;

        const auto m1 = (*measurer)->measure_font(a);
        const auto m2 = (*measurer)->measure_font(b);
        if (!m1 || !m2)
        {
            fwprintf(stderr, L"[DETAIL] measure_font failed (err=%lu/%lu)\n",
                m1 ? 0ul : oc::core::to_dword(m1.error()),
                m2 ? 0ul : oc::core::to_dword(m2.error()));
            return false;
        }

        const int w1 = m1->cell.width_px;
        const int h1 = m1->cell.height_px;
        const int w2 = m2->cell.width_px;
        const int h2 = m2->cell.height_px;

        const int expected_w2 = w1 * 2;
        const int expected_h2 = h1 * 2;
        if (std::abs(w2 - expected_w2) > 1 || std::abs(h2 - expected_h2) > 1)
        {
            fwprintf(stderr, L"[DETAIL] point-size scaling not ~2x (12pt=%dx%d 24pt=%dx%d)\n", w1, h1, w2, h2);
            return false;
        }

        return true;
    }
}

bool run_dwrite_text_measurer_tests()
{
    const CoInitScope coinit(::CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    if (FAILED(coinit.result()) && coinit.result() != RPC_E_CHANGED_MODE)
    {
        fwprintf(stderr, L"[DETAIL] CoInitializeEx failed (hr=0x%08X)\n", static_cast<unsigned>(coinit.result()));
        return false;
    }

    if (!test_measure_consolas_basic())
    {
        return false;
    }

    if (!test_missing_font_falls_back_to_consolas())
    {
        return false;
    }

    if (!test_dpi_scaling_is_linear())
    {
        return false;
    }

    if (!test_point_size_scaling_is_linear())
    {
        return false;
    }

    return true;
}
