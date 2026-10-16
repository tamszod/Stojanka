#include "bot/session_watch.h"

#include <atomic>
#include <future>
#include <thread>

#include "core/log.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace strojanka {

#if defined(_WIN32)

namespace {
std::atomic<SessionWatch::Impl*> g_active{nullptr};
constexpr const wchar_t* kWindowClass = L"StrojankaSessionWatch";
}  // namespace

struct SessionWatch::Impl {
    OnEnd on_end;
    std::atomic<bool> fired{false};
    HWND hwnd = nullptr;
    std::thread thread;

    void end(const std::string& reason, bool forced) {
        if (fired.exchange(true)) return;
        log_warn(reason + ": stopping Strojanka");
        on_end(reason, forced);
    }

    static std::string end_session_reason(LPARAM flags) {
        std::string reason;
        if (flags & ENDSESSION_CLOSEAPP) reason = "an installer or Windows Update asked running programs to close";
        else if (flags & ENDSESSION_LOGOFF) reason = "the Windows user is signing out";
        else reason = "Windows is shutting down or restarting";
        if (flags & ENDSESSION_CRITICAL) reason += " (forced)";
        return reason;
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wp && self) self->end(end_session_reason(lp), true);
                return 0;
            case WM_POWERBROADCAST:
                if (wp == PBT_APMSUSPEND) log_info("the computer is going to sleep");
                else if (wp == PBT_APMRESUMEAUTOMATIC) log_info("the computer woke up from sleep");
                return TRUE;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static BOOL WINAPI console_handler(DWORD event) {
        Impl* self = g_active.load();
        if (!self) return FALSE;
        switch (event) {
            case CTRL_C_EVENT:
            case CTRL_BREAK_EVENT: self->end("Ctrl+C was pressed", false); return TRUE;
            case CTRL_CLOSE_EVENT: self->end("the console window was closed", true); return TRUE;
            case CTRL_LOGOFF_EVENT: self->end("the Windows user is signing out", true); return TRUE;
            case CTRL_SHUTDOWN_EVENT: self->end("Windows is shutting down or restarting", true); return TRUE;
        }
        return FALSE;
    }

    // A hidden top-level window: message-only windows don't receive the session broadcasts.
    void run(std::promise<void>& ready) {
        HINSTANCE inst = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = wnd_proc;
        wc.hInstance = inst;
        wc.lpszClassName = kWindowClass;
        RegisterClassExW(&wc);  // fails harmlessly if already registered
        hwnd = CreateWindowExW(0, kWindowClass, L"Strojanka", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        ready.set_value();
        if (!hwnd) {
            log_warn("could not watch for Windows shutting down (CreateWindow failed)");
            return;
        }
        MSG m;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
};

SessionWatch::SessionWatch(OnEnd on_end) : impl_(std::make_unique<Impl>()) {
    impl_->on_end = std::move(on_end);
    std::promise<void> ready;
    auto started = ready.get_future();
    impl_->thread = std::thread([this, &ready] { impl_->run(ready); });
    started.wait();
    g_active = impl_.get();
    SetConsoleCtrlHandler(Impl::console_handler, TRUE);
}

SessionWatch::~SessionWatch() {
    SetConsoleCtrlHandler(Impl::console_handler, FALSE);
    g_active = nullptr;
    if (impl_->hwnd) PostMessageW(impl_->hwnd, WM_CLOSE, 0, 0);
    if (impl_->thread.joinable()) impl_->thread.join();
}

#else

struct SessionWatch::Impl {};

SessionWatch::SessionWatch(OnEnd) {}
SessionWatch::~SessionWatch() = default;

#endif

}  // namespace strojanka
