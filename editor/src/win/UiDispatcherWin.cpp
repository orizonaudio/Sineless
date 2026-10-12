#include "editor/UiDispatcher.h"

// windows.h first: the other Windows headers depend on it.
// clang-format off
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// clang-format on
#include "WinModule.h"

#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace orizon::editor {
namespace {

// The posted-work queue has new entries.
constexpr UINT kWorkMessage = WM_APP + 1;

// A hidden message-only window on the UI thread runs posted work and timers. Destroying it
// drops whatever is still pending, so nothing runs after the dispatcher is gone.
class UiDispatcherWin final : public UiDispatcher
{
public:
    UiDispatcherWin()
    {
        static const wchar_t* const kClassName = L"OrizonSinelessUiDispatcher";
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &UiDispatcherWin::windowProcedure;
        windowClass.hInstance = thisModule();
        windowClass.lpszClassName = kClassName;
        RegisterClassExW(&windowClass); // Fails harmlessly once the class exists.
        window = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                 windowClass.hInstance, nullptr);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    ~UiDispatcherWin() override
    {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        DestroyWindow(window); // Also stops its timers.
    }

    UiDispatcherWin(const UiDispatcherWin&) = delete;
    UiDispatcherWin& operator=(const UiDispatcherWin&) = delete;

    void post(std::function<void()> work) override
    {
        bool wake = false;
        {
            const std::scoped_lock lock(mutex);
            wake = pending.empty();
            pending.push_back(std::move(work));
        }
        if (wake) {
            PostMessageW(window, kWorkMessage, 0, 0);
        }
    }

    void every(std::chrono::milliseconds interval, std::function<void()> work) override
    {
        timers.push_back(std::move(work));
        SetTimer(window, timers.size(), static_cast<UINT>(interval.count()), nullptr);
    }

private:
    static LRESULT CALLBACK windowProcedure(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<UiDispatcherWin*>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (self != nullptr) {
            if (message == kWorkMessage) {
                self->runPending();
                return 0;
            }
            if (message == WM_TIMER && wParam >= 1 && wParam <= self->timers.size()) {
                self->timers[wParam - 1]();
                return 0;
            }
        }
        return DefWindowProcW(handle, message, wParam, lParam);
    }

    void runPending()
    {
        std::deque<std::function<void()>> work;
        {
            const std::scoped_lock lock(mutex);
            work.swap(pending);
        }
        for (auto& task : work) {
            task();
        }
    }

    HWND window = nullptr;
    std::mutex mutex;
    std::deque<std::function<void()>> pending; // Guarded by `mutex`.
    std::vector<std::function<void()>> timers; // UI thread only; timer id = index + 1.
};

} // namespace

std::unique_ptr<UiDispatcher> UiDispatcher::create()
{
    return std::make_unique<UiDispatcherWin>();
}

} // namespace orizon::editor
