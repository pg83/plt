#include "platform_win32.h"
#include "platform_win32_logic.h"

#include "input.h"
#include "poller.h"
#include "window.h"
#include "platform.h"

#include <std/sys/crt.h>
#include <std/sys/throw.h>
#include <std/sym/i_map.h>
#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/thr/poll_fd.h>
#include <std/mem/obj_pool.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <io.h>
#include <shellapi.h>
#include <windowsx.h>

#include <errno.h>
#include <limits.h>

using namespace stl;
using namespace plt;

namespace {
    constexpr UINT frameMessage = WM_APP + 0x317;
    constexpr UINT nonMutatingUnicodeTranslation = 1u << 2;
    constexpr u16 cursorIBeam = 32513;
    constexpr u16 cursorHand = 32649;

    struct PlatformImpl;
    struct PollerImpl;
    struct WindowImpl;

    HCURSOR loadCursor(u16 id) {
        return LoadCursorW(nullptr, MAKEINTRESOURCEW(id));
    }

    [[noreturn]]
    void fail(StringView message) {
        Errno(EIO).raise(message);
    }

    struct ArmedFD {
        PollFD fd;
        PollCallback* callback = nullptr;
        HANDLE handle = nullptr;
    };

    struct ReadyFD {
        PollFD fd;
        PollCallback* callback = nullptr;
    };

    struct Timer {
        TimerCallback* callback = nullptr;
        u64 deadline = 0;
    };

    struct PollerImpl final: public Poller {
        explicit PollerImpl(ObjPool& owner);

        void arm(PollFD fd, PollCallback& callback) override;
        void disarm(int fd) override;
        void timeout(u64 microseconds, TimerCallback& callback) override;
        void deadline(u64 monotonicMicroseconds, TimerCallback& callback) override;
        void cancel(TimerCallback& callback) override;

        void prepare();
        void dispatchHandles(DWORD result);
        void dispatchTimers();
        DWORD waitMilliseconds() const;
        u64 nextDeadline() const;

        IntMap<ArmedFD> armed;
        Vector<HANDLE> handles;
        Vector<int> handleFDs;
        Vector<ReadyFD> readyFDs;
        Vector<Timer> timers;
        Vector<TimerCallback*> readyTimers;
    };

    Buffer wideString(StringView value) {
        Buffer result;
        if (value.empty()) {
            result.zero(sizeof(wchar_t));
            return result;
        }
        const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)(value.data()), (int)(value.length()), nullptr, 0);
        if (length <= 0) {
            result.zero(sizeof(wchar_t));
            return result;
        }
        result.grow((length + 1) * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char*)(value.data()), (int)(value.length()), (wchar_t*)(result.mutData()), length);
        ((wchar_t*)(result.mutData()))[length] = 0;
        result.seekAbsolute((length + 1) * sizeof(wchar_t));
        return result;
    }

    void appendUtf8(Buffer& output, const wchar_t* value, int length) {
        if (length <= 0) {
            return;
        }
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) {
            return;
        }
        const size_t offset = output.used();
        output.grow(offset + bytes);
        WideCharToMultiByte(CP_UTF8, 0, value, length, (char*)(output.mutData()) + offset, bytes, nullptr, nullptr);
        output.seekAbsolute(offset + bytes);
    }

    struct WindowImpl final: public Window {
        WindowImpl(PlatformImpl& platform, const WindowOptions& options);
        ~WindowImpl();

        void show() override;
        void requestClose() override;
        bool requestFrame() override;
        void cancelFrame() override;
        void setTitle(StringView title) override;
        void requestAttention() override;
        void requestRedraw() override;
        void restore() override;
        void iconify() override;
        void move(i32 x, i32 y) override;
        void focus() override;
        void setMaximized(bool maximized) override;
        void setFullscreen(bool fullscreen) override;
        void resize(u32 width, u32 height) override;
        void setMinimumSize(u32 width, u32 height) override;
        void setResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) override;
        WindowInfo info() const override;
        void readPrimary(ClipboardRead& read) override;
        void readClipboard(ClipboardRead& read) override;
        void cancelClipboardRead(ClipboardRead& read) override;
        void writePrimary(StringView content) override;
        void writeClipboard(StringView content) override;
        void pointerIcon(PointerIcon icon) override;
        RenderContext renderContext() const override;

        LRESULT message(UINT message, WPARAM wparam, LPARAM lparam);
        void resized();
        void key(WPARAM key, LPARAM data, bool pressed);
        void textInput(WPARAM value);
        void pointerMotion(LPARAM data);
        void pointerButton(UINT message, WPARAM state, LPARAM data);
        void scroll(UINT message, WPARAM state, LPARAM data);
        void frame();
        void snapRect(RECT& rect, UINT edge);
        void completeClipboardRead(ClipboardRead& read, StringView content, bool success);
        InputKey inputKey(WPARAM key, LPARAM data) const;
        u16 modifiers() const;
        u32 layoutCodepoint(WPARAM key, LPARAM data, bool base) const;
        PointerButton pointerButtonFor(UINT message) const;
        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

        PlatformImpl& platform;
        InputSink* input = nullptr;
        WindowEvents* events = nullptr;
        HWND handle = nullptr;
        Buffer primary;
        Buffer clipboard;
        Vector<ClipboardRead*> clipboardReads;
        u32 minimumWidth = 1;
        u32 minimumHeight = 1;
        u32 resizeUnitWidth = 1;
        u32 resizeUnitHeight = 1;
        u32 resizeBaseWidth = 0;
        u32 resizeBaseHeight = 0;
        wchar_t highSurrogate = 0;
        HCURSOR cursor = nullptr;
        WINDOWPLACEMENT placement{};
        LONG_PTR windowedStyle = 0;
        unsigned pressedButtons = 0;
        bool framePending = false;
        bool fullscreen = false;
        bool pointerPresent = false;
    };

    struct PlatformImpl final: public Platform {
        explicit PlatformImpl(ObjPool& owner);
        ~PlatformImpl();

        Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
        Poller* poller() override;
        void run() override;
        void stop() override;

        void dispatchMessages();
        void queueInputFlush(WindowImpl& window);
        void forget(WindowImpl& window);

        PollerImpl* poller_ = nullptr;
        HINSTANCE instance = nullptr;
        DWORD thread = 0;
        ATOM windowClass = 0;
        Vector<WindowImpl*> pendingInputFlushes;
        bool dispatchingMessages = false;
        bool running = false;
    };

    const wchar_t* className = L"pg83.plt.window";
}

PlatformImpl::PlatformImpl(ObjPool& owner)
    : poller_(owner.make<PollerImpl>(owner))
    , instance(GetModuleHandleW(nullptr))
    , thread(GetCurrentThreadId())
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW description{};
    description.cbSize = sizeof(description);
    description.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    description.lpfnWndProc = WindowImpl::procedure;
    description.hInstance = instance;
    description.hCursor = loadCursor(cursorIBeam);
    description.lpszClassName = className;
    windowClass = RegisterClassExW(&description);
    if (windowClass == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        fail(u8"RegisterClassExW failed");
    }
}

PlatformImpl::~PlatformImpl() {
    if (windowClass != 0) {
        UnregisterClassW(className, instance);
    }
}

Window* PlatformImpl::createWindow(ObjPool& owner, const WindowOptions& options) {
    return owner.make<WindowImpl>(*this, options);
}

Poller* PlatformImpl::poller() {
    return poller_;
}

PollerImpl::PollerImpl(ObjPool& owner)
    : armed(ObjPool::create(&owner))
{
}

void PollerImpl::arm(PollFD fd, PollCallback& callback) {
    const intptr_t native = _get_osfhandle(fd.fd);
    armed[fd.fd] = {
        .fd = fd,
        .callback = &callback,
        .handle = native == -1 ? nullptr : (HANDLE)(native),
    };
}

void PollerImpl::disarm(int fd) {
    armed.erase(fd);
}

void PollerImpl::timeout(u64 microseconds, TimerCallback& callback) {
    deadline(monotonicNowUs() + microseconds, callback);
}

void PollerImpl::deadline(u64 monotonicMicroseconds, TimerCallback& callback) {
    if (monotonicMicroseconds == 0) {
        monotonicMicroseconds = monotonicNowUs();
    }
    for (Timer* timer = timers.mutBegin(); timer != timers.mutEnd(); ++timer) {
        if (timer->callback == &callback) {
            timer->deadline = monotonicMicroseconds;
            return;
        }
    }
    timers.pushBack({
        .callback = &callback,
        .deadline = monotonicMicroseconds,
    });
}

void PollerImpl::cancel(TimerCallback& callback) {
    for (size_t index = 0; index != timers.length(); ++index) {
        if (timers[index].callback == &callback) {
            timers.mut(index) = timers.back();
            timers.popBack();
            return;
        }
    }
}

u64 PollerImpl::nextDeadline() const {
    u64 result = UINT64_MAX;
    for (const Timer& timer : timers) {
        result = min(result, timer.deadline);
    }
    return result;
}

DWORD PollerImpl::waitMilliseconds() const {
    const u64 deadline = nextDeadline();
    if (deadline == UINT64_MAX) {
        return INFINITE;
    }
    const u64 now = monotonicNowUs();
    if (deadline <= now) {
        return 0;
    }
    return (DWORD)(min<u64>(UINT_MAX - 1, (deadline - now + 999) / 1000));
}

void PollerImpl::prepare() {
    handles.clear();
    handleFDs.clear();
    armed.visit([this](const ArmedFD& source) {
        if (source.handle != nullptr && handles.length() < MAXIMUM_WAIT_OBJECTS - 1) {
            handles.pushBack(source.handle);
            handleFDs.pushBack(source.fd.fd);
        }
    });
}

void PollerImpl::dispatchHandles(DWORD result) {
    readyFDs.clear();
    if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + handles.length()) {
        const size_t first = result - WAIT_OBJECT_0;
        for (size_t index = first; index != handles.length(); ++index) {
            if (index != first && WaitForSingleObject(handles[index], 0) != WAIT_OBJECT_0) {
                continue;
            }
            const int fd = handleFDs[index];
            ArmedFD* const source = armed.find(fd);
            if (source != nullptr) {
                readyFDs.pushBack({
                    .fd = source->fd,
                    .callback = source->callback,
                });
                armed.erase(fd);
            }
        }
    }
    for (const ReadyFD& ready : readyFDs) {
        ready.callback->ready(ready.fd);
    }
    readyFDs.clear();
}

void PollerImpl::dispatchTimers() {
    const u64 now = monotonicNowUs();
    readyTimers.clear();
    for (size_t index = 0; index != timers.length();) {
        if (timers[index].deadline > now) {
            ++index;
            continue;
        }
        readyTimers.pushBack(timers[index].callback);
        timers.mut(index) = timers.back();
        timers.popBack();
    }
    for (TimerCallback* callback : readyTimers) {
        callback->ready();
    }
    readyTimers.clear();
}

void PlatformImpl::dispatchMessages() {
    dispatchingMessages = true;
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            running = false;
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    dispatchingMessages = false;
    while (!pendingInputFlushes.empty()) {
        WindowImpl* const window = pendingInputFlushes.back();
        pendingInputFlushes.popBack();
        if (window->input != nullptr) {
            window->input->flush();
        }
    }
}

void PlatformImpl::queueInputFlush(WindowImpl& window) {
    if (!dispatchingMessages) {
        if (window.input != nullptr) {
            window.input->flush();
        }
        return;
    }
    for (WindowImpl* pending : pendingInputFlushes) {
        if (pending == &window) {
            return;
        }
    }
    pendingInputFlushes.pushBack(&window);
}

void PlatformImpl::forget(WindowImpl& window) {
    for (size_t index = 0; index != pendingInputFlushes.length(); ++index) {
        if (pendingInputFlushes[index] == &window) {
            pendingInputFlushes.mut(index) = pendingInputFlushes.back();
            pendingInputFlushes.popBack();
            return;
        }
    }
}

void PlatformImpl::run() {
    running = true;
    while (running) {
        poller_->prepare();
        const DWORD result = MsgWaitForMultipleObjectsEx((DWORD)(poller_->handles.length()), poller_->handles.data(), poller_->waitMilliseconds(), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        poller_->dispatchHandles(result);
        dispatchMessages();
        poller_->dispatchTimers();
    }
}

void PlatformImpl::stop() {
    running = false;
    PostThreadMessageW(thread, WM_NULL, 0, 0);
}

WindowImpl::WindowImpl(PlatformImpl& platform_, const WindowOptions& options)
    : platform(platform_)
    , input(options.input)
    , events(options.events)
    , minimumWidth(max(1u, options.minimumWidth))
    , minimumHeight(max(1u, options.minimumHeight))
{
    placement.length = sizeof(placement);
    Buffer title = wideString(options.title);
    RECT area{0, 0, (LONG)(max(1u, options.width)), (LONG)(max(1u, options.height))};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectExForDpi(&area, style, FALSE, 0, USER_DEFAULT_SCREEN_DPI);
    handle = CreateWindowExW(0, className, (const wchar_t*)(title.data()), style, CW_USEDEFAULT, CW_USEDEFAULT, area.right - area.left, area.bottom - area.top, nullptr, nullptr, platform.instance, this);
    if (handle == nullptr) {
        fail(u8"CreateWindowExW failed");
    }
    cursor = loadCursor(cursorIBeam);
}

WindowImpl::~WindowImpl() {
    platform.forget(*this);
    if (handle != nullptr) {
        SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
        DestroyWindow(handle);
    }
}

void WindowImpl::show() {
    ShowWindow(handle, SW_SHOW);
    UpdateWindow(handle);
    resized();
}

void WindowImpl::requestClose() {
    PostMessageW(handle, WM_CLOSE, 0, 0);
}

bool WindowImpl::requestFrame() {
    if (framePending) {
        return true;
    }
    framePending = PostMessageW(handle, frameMessage, 0, 0) != FALSE;
    return framePending;
}

void WindowImpl::cancelFrame() {
    framePending = false;
}

void WindowImpl::setTitle(StringView value) {
    Buffer title = wideString(value);
    SetWindowTextW(handle, (const wchar_t*)(title.data()));
}

void WindowImpl::requestAttention() {
    FLASHWINFO flash{};
    flash.cbSize = sizeof(flash);
    flash.hwnd = handle;
    flash.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    flash.uCount = 3;
    FlashWindowEx(&flash);
}

void WindowImpl::requestRedraw() {
    InvalidateRect(handle, nullptr, FALSE);
}

void WindowImpl::restore() {
    ShowWindow(handle, SW_RESTORE);
    if (fullscreen) {
        setFullscreen(false);
    }
}

void WindowImpl::iconify() {
    ShowWindow(handle, SW_MINIMIZE);
}

void WindowImpl::move(i32 x, i32 y) {
    SetWindowPos(handle, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void WindowImpl::focus() {
    ShowWindow(handle, SW_RESTORE);
    SetForegroundWindow(handle);
    SetFocus(handle);
}

void WindowImpl::setMaximized(bool value) {
    ShowWindow(handle, value ? SW_MAXIMIZE : SW_RESTORE);
}

void WindowImpl::setFullscreen(bool value) {
    if (fullscreen == value) {
        return;
    }
    if (value) {
        placement.length = sizeof(placement);
        GetWindowPlacement(handle, &placement);
        windowedStyle = GetWindowLongPtrW(handle, GWL_STYLE);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
        SetWindowLongPtrW(handle, GWL_STYLE, windowedStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(handle, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top, monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOACTIVATE);
    } else {
        SetWindowLongPtrW(handle, GWL_STYLE, windowedStyle);
        SetWindowPlacement(handle, &placement);
        SetWindowPos(handle, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    fullscreen = value;
}

void WindowImpl::resize(u32 width, u32 height) {
    RECT area{0, 0, (LONG)(max(1u, width)), (LONG)(max(1u, height))};
    const DWORD style = (DWORD)(GetWindowLongPtrW(handle, GWL_STYLE));
    const DWORD extended = (DWORD)(GetWindowLongPtrW(handle, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&area, style, FALSE, extended, GetDpiForWindow(handle));
    SetWindowPos(handle, nullptr, 0, 0, area.right - area.left, area.bottom - area.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void WindowImpl::setMinimumSize(u32 width, u32 height) {
    minimumWidth = max(1u, width);
    minimumHeight = max(1u, height);
}

void WindowImpl::setResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
    resizeUnitWidth = max(1u, width);
    resizeUnitHeight = max(1u, height);
    resizeBaseWidth = baseWidth;
    resizeBaseHeight = baseHeight;
}

WindowInfo WindowImpl::info() const {
    RECT client;
    GetClientRect(handle, &client);
    RECT window;
    GetWindowRect(handle, &window);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromWindow(handle, MONITOR_DEFAULTTONEAREST), &monitor);
    const u32 dpi = GetDpiForWindow(handle);
    return {
        .x = window.left,
        .y = window.top,
        .width = (u32)(max(0L, client.right - client.left)),
        .height = (u32)(max(0L, client.bottom - client.top)),
        .screenPixelWidth = (u32)(max(0L, monitor.rcMonitor.right - monitor.rcMonitor.left)),
        .screenPixelHeight = (u32)(max(0L, monitor.rcMonitor.bottom - monitor.rcMonitor.top)),
        .contentScale = dpi / (float)(USER_DEFAULT_SCREEN_DPI),
        .focused = GetForegroundWindow() == handle,
        .iconified = IsIconic(handle) != FALSE,
        .maximized = IsZoomed(handle) != FALSE,
        .fullscreen = fullscreen,
    };
}

void WindowImpl::completeClipboardRead(ClipboardRead& read, StringView content, bool success) {
    const size_t slot = clipboardReads.length();
    clipboardReads.pushBack(&read);
    if (success && !content.empty() && !read.data(content)) {
        success = false;
    }
    ClipboardRead* const completion = clipboardReads[slot];
    clipboardReads.popBack();
    if (completion != nullptr) {
        completion->done(success);
    }
}

void WindowImpl::readPrimary(ClipboardRead& read) {
    completeClipboardRead(read, StringView(primary), true);
}

void WindowImpl::readClipboard(ClipboardRead& read) {
    clipboard.reset();
    if (!OpenClipboard(handle)) {
        read.done(false);
        return;
    }
    bool success = false;
    HANDLE data = GetClipboardData(CF_UNICODETEXT);
    if (data != nullptr) {
        const wchar_t* const value = (const wchar_t*)(GlobalLock(data));
        if (value != nullptr) {
            const size_t capacity = GlobalSize(data) / sizeof(wchar_t);
            const size_t length = win32_detail::boundedWideLength(value, capacity);
            appendUtf8(clipboard, value, (int)(min<size_t>(length, INT_MAX)));
            success = true;
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    completeClipboardRead(read, StringView(clipboard), success);
}

void WindowImpl::cancelClipboardRead(ClipboardRead& read) {
    for (size_t index = 0; index != clipboardReads.length(); ++index) {
        if (clipboardReads[index] == &read) {
            clipboardReads.mut(index) = nullptr;
        }
    }
}

void WindowImpl::writePrimary(StringView content) {
    primary.reset();
    primary.append(content.data(), content.length());
}

void WindowImpl::writeClipboard(StringView content) {
    Buffer value = wideString(content);
    if (!OpenClipboard(handle)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = value.used();
    HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (storage != nullptr) {
        void* target = GlobalLock(storage);
        if (target != nullptr) {
            memCpy(target, value.data(), bytes);
            GlobalUnlock(storage);
            if (SetClipboardData(CF_UNICODETEXT, storage) == nullptr) {
                GlobalFree(storage);
            }
        } else {
            GlobalFree(storage);
        }
    }
    CloseClipboard();
}

void WindowImpl::pointerIcon(PointerIcon icon) {
    cursor = loadCursor(icon == PointerIcon::Link ? cursorHand : cursorIBeam);
    SetCursor(cursor);
}

RenderContext WindowImpl::renderContext() const {
    return {
        .backend = RenderBackend::Win32,
        .connection = platform.instance,
        .window = handle,
    };
}

u16 WindowImpl::modifiers() const {
    u16 result = 0;
    if (GetKeyState(VK_SHIFT) < 0) {
        result |= InputShift;
    }
    if (GetKeyState(VK_CONTROL) < 0) {
        result |= InputControl;
    }
    if (GetKeyState(VK_MENU) < 0) {
        result |= InputAlt;
    }
    if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) {
        result |= InputSuper;
    }
    if (GetKeyState(VK_CAPITAL) & 1) {
        result |= InputCapsLock;
    }
    if (GetKeyState(VK_NUMLOCK) & 1) {
        result |= InputNumLock;
    }
    if (GetKeyState(VK_RMENU) < 0) {
        result |= InputAltGraph;
    }
    return result;
}

InputKey WindowImpl::inputKey(WPARAM key, LPARAM data) const {
    if ((key >= '0' && key <= '9') || (key >= 'A' && key <= 'Z') || key == VK_SPACE || (key >= VK_OEM_1 && key <= VK_OEM_8)) {
        return InputKey::Printable;
    }
    switch (key) {
        case VK_ESCAPE:
            return InputKey::Escape;
        case VK_RETURN:
            return win32_detail::extendedKey(data) ? InputKey::KeypadEnter : InputKey::Enter;
        case VK_BACK:
            return InputKey::Backspace;
        case VK_TAB:
            return InputKey::Tab;
        case VK_INSERT:
            return InputKey::Insert;
        case VK_DELETE:
            return InputKey::Delete;
        case VK_HOME:
            return InputKey::Home;
        case VK_END:
            return InputKey::End;
        case VK_UP:
            return InputKey::Up;
        case VK_DOWN:
            return InputKey::Down;
        case VK_LEFT:
            return InputKey::Left;
        case VK_RIGHT:
            return InputKey::Right;
        case VK_PRIOR:
            return InputKey::PageUp;
        case VK_NEXT:
            return InputKey::PageDown;
        case VK_F1:
        case VK_F2:
        case VK_F3:
        case VK_F4:
        case VK_F5:
        case VK_F6:
        case VK_F7:
        case VK_F8:
        case VK_F9:
        case VK_F10:
        case VK_F11:
        case VK_F12:
        case VK_F13:
        case VK_F14:
        case VK_F15:
        case VK_F16:
        case VK_F17:
        case VK_F18:
        case VK_F19:
        case VK_F20:
            return (InputKey)((u8)(InputKey::F1) + key - VK_F1);
        case VK_NUMPAD0:
        case VK_NUMPAD1:
        case VK_NUMPAD2:
        case VK_NUMPAD3:
        case VK_NUMPAD4:
        case VK_NUMPAD5:
        case VK_NUMPAD6:
        case VK_NUMPAD7:
        case VK_NUMPAD8:
        case VK_NUMPAD9:
            return (InputKey)((u8)(InputKey::Keypad0) + key - VK_NUMPAD0);
        case VK_DECIMAL:
            return InputKey::KeypadDecimal;
        case VK_DIVIDE:
            return InputKey::KeypadDivide;
        case VK_MULTIPLY:
            return InputKey::KeypadMultiply;
        case VK_SUBTRACT:
            return InputKey::KeypadSubtract;
        case VK_ADD:
            return InputKey::KeypadAdd;
        case VK_CAPITAL:
            return InputKey::CapsLock;
        case VK_SCROLL:
            return InputKey::ScrollLock;
        case VK_NUMLOCK:
            return InputKey::NumLock;
        case VK_SNAPSHOT:
            return InputKey::PrintScreen;
        case VK_PAUSE:
            return InputKey::Pause;
        case VK_APPS:
            return InputKey::Menu;
        case VK_LSHIFT:
            return InputKey::LeftShift;
        case VK_LCONTROL:
            return InputKey::LeftControl;
        case VK_LMENU:
            return InputKey::LeftAlt;
        case VK_LWIN:
            return InputKey::LeftSuper;
        case VK_RSHIFT:
            return InputKey::RightShift;
        case VK_RCONTROL:
            return InputKey::RightControl;
        case VK_RMENU:
            return InputKey::RightAlt;
        case VK_RWIN:
            return InputKey::RightSuper;
        default:
            return InputKey::Unknown;
    }
}

u32 WindowImpl::layoutCodepoint(WPARAM key, LPARAM data, bool base) const {
    BYTE state[256]{};
    if (!base) {
        GetKeyboardState(state);
    }
    wchar_t output[4]{};
    const UINT scan = (data >> 16) & 0xff;
    const int count = ToUnicodeEx((UINT)(key), scan, state, output, 4, nonMutatingUnicodeTranslation, GetKeyboardLayout(0));
    if (count <= 0) {
        return 0;
    }
    if (count > 1 && output[0] >= 0xd800 && output[0] <= 0xdbff && output[1] >= 0xdc00 && output[1] <= 0xdfff) {
        return 0x10000 + (((u32)(output[0] - 0xd800)) << 10) + output[1] - 0xdc00;
    }
    return output[0];
}

void WindowImpl::key(WPARAM key, LPARAM data, bool pressed) {
    if (input == nullptr) {
        return;
    }
    if (key == VK_SHIFT) {
        key = MapVirtualKeyW((data >> 16) & 0xff, MAPVK_VSC_TO_VK_EX);
    } else if (key == VK_CONTROL) {
        key = data & (1u << 24) ? VK_RCONTROL : VK_LCONTROL;
    } else if (key == VK_MENU) {
        key = data & (1u << 24) ? VK_RMENU : VK_LMENU;
    }
    input->key({
        .key = inputKey(key, data),
        .action = !pressed ? InputAction::Release : (data & (1u << 30) ? InputAction::Repeat : InputAction::Press),
        .modifiers = modifiers(),
        .layoutCodepoint = layoutCodepoint(key, data, false),
        .baseCodepoint = layoutCodepoint(key, data, true),
    });
}

void WindowImpl::textInput(WPARAM value) {
    if (input == nullptr) {
        return;
    }
    u32 codepoint = (u32)(value);
    if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
        highSurrogate = (wchar_t)(codepoint);
        return;
    }
    if (codepoint >= 0xdc00 && codepoint <= 0xdfff && highSurrogate != 0) {
        codepoint = 0x10000 + (((u32)(highSurrogate - 0xd800)) << 10) + codepoint - 0xdc00;
    }
    highSurrogate = 0;
    input->text({codepoint, modifiers()});
}

void WindowImpl::pointerMotion(LPARAM data) {
    if (!pointerPresent) {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_LEAVE;
        tracking.hwndTrack = handle;
        TrackMouseEvent(&tracking);
        pointerPresent = true;
        if (input != nullptr) {
            input->pointerPresence(true);
        }
    }
    if (input != nullptr) {
        input->pointerMotion({GET_X_LPARAM(data), GET_Y_LPARAM(data), modifiers()});
    }
}

PointerButton WindowImpl::pointerButtonFor(UINT message) const {
    switch (message) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return PointerButton::Primary;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return PointerButton::Secondary;
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
            return PointerButton::Middle;
        default:
            return PointerButton::Auxiliary1;
    }
}

void WindowImpl::pointerButton(UINT message, WPARAM state, LPARAM data) {
    if (input == nullptr) {
        return;
    }
    const bool pressed = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
    PointerButton button = pointerButtonFor(message);
    if (message == WM_XBUTTONDOWN || message == WM_XBUTTONUP) {
        button = GET_XBUTTON_WPARAM(state) == XBUTTON1 ? PointerButton::Auxiliary1 : PointerButton::Auxiliary2;
    }
    const unsigned buttonMask = 1u << (u8)(button);
    switch (win32_detail::updateButtonMask(pressedButtons, buttonMask, pressed)) {
        case win32_detail::CaptureChange::Acquire:
            SetCapture(handle);
            break;
        case win32_detail::CaptureChange::Release:
            ReleaseCapture();
            break;
        case win32_detail::CaptureChange::None:
            break;
    }
    input->pointerButton({
        .button = button,
        .pressed = pressed,
        .pixelX = GET_X_LPARAM(data),
        .pixelY = GET_Y_LPARAM(data),
        .modifiers = modifiers(),
        .time = GetMessageTime() / 1000.0,
    });
}

void WindowImpl::scroll(UINT message, WPARAM state, LPARAM data) {
    if (input == nullptr) {
        return;
    }
    POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)};
    ScreenToClient(handle, &point);
    const double value = GET_WHEEL_DELTA_WPARAM(state) / (double)(WHEEL_DELTA);
    input->scroll({
        .x = message == WM_MOUSEHWHEEL ? value : 0,
        .y = message == WM_MOUSEWHEEL ? value : 0,
        .pixelX = point.x,
        .pixelY = point.y,
        .modifiers = modifiers(),
    });
}

void WindowImpl::resized() {
    if (events != nullptr && !IsIconic(handle)) {
        events->resized(info());
    }
}

void WindowImpl::frame() {
    if (!framePending) {
        return;
    }
    DwmFlush();
    framePending = false;
    if (events != nullptr) {
        events->frame();
    }
}

void WindowImpl::snapRect(RECT& rect, UINT edge) {
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, (DWORD)(GetWindowLongPtrW(handle, GWL_STYLE)), FALSE, (DWORD)(GetWindowLongPtrW(handle, GWL_EXSTYLE)), GetDpiForWindow(handle));
    const i32 decorationWidth = frame.right - frame.left;
    const i32 decorationHeight = frame.bottom - frame.top;
    i32 width = rect.right - rect.left - decorationWidth;
    i32 height = rect.bottom - rect.top - decorationHeight;
    if (resizeUnitWidth > 1 && width > (i32)(resizeBaseWidth)) {
        width = resizeBaseWidth + ((width - resizeBaseWidth) / resizeUnitWidth) * resizeUnitWidth;
    }
    if (resizeUnitHeight > 1 && height > (i32)(resizeBaseHeight)) {
        height = resizeBaseHeight + ((height - resizeBaseHeight) / resizeUnitHeight) * resizeUnitHeight;
    }
    width += decorationWidth;
    height += decorationHeight;
    if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) {
        rect.left = rect.right - width;
    } else {
        rect.right = rect.left + width;
    }
    if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) {
        rect.top = rect.bottom - height;
    } else {
        rect.bottom = rect.top + height;
    }
}

LRESULT WindowImpl::message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CLOSE:
            if (events != nullptr) {
                events->close();
            }
            return 0;
        case WM_SIZE:
            resized();
            return 0;
        case WM_DPICHANGED: {
            const RECT& rect = *(const RECT*)(lparam);
            SetWindowPos(handle, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint;
            BeginPaint(handle, &paint);
            EndPaint(handle, &paint);
            if (events != nullptr) {
                events->redraw();
            }
            return 0;
        }
        case WM_SETFOCUS:
            if (input != nullptr) {
                input->focus(true);
            }
            return 0;
        case WM_KILLFOCUS:
            if (input != nullptr) {
                input->focus(false);
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            key(wparam, lparam, true);
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            key(wparam, lparam, false);
            return 0;
        case WM_CHAR:
        case WM_SYSCHAR:
            textInput(wparam);
            return 0;
        case WM_MOUSEMOVE:
            pointerMotion(lparam);
            return 0;
        case WM_MOUSELEAVE:
            pointerPresent = false;
            if (input != nullptr) {
                input->pointerPresence(false);
            }
            return 0;
        case WM_CAPTURECHANGED:
            pressedButtons = 0;
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            pointerButton(message, wparam, lparam);
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            scroll(message, wparam, lparam);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT) {
                SetCursor(cursor);
                return TRUE;
            }
            break;
        case WM_GETMINMAXINFO: {
            MINMAXINFO& bounds = *(MINMAXINFO*)(lparam);
            RECT area{0, 0, (LONG)(minimumWidth), (LONG)(minimumHeight)};
            AdjustWindowRectExForDpi(&area, (DWORD)(GetWindowLongPtrW(handle, GWL_STYLE)), FALSE, (DWORD)(GetWindowLongPtrW(handle, GWL_EXSTYLE)), GetDpiForWindow(handle));
            bounds.ptMinTrackSize.x = area.right - area.left;
            bounds.ptMinTrackSize.y = area.bottom - area.top;
            return 0;
        }
        case WM_SIZING:
            snapRect(*(RECT*)(lparam), (UINT)(wparam));
            return TRUE;
        case frameMessage:
            frame();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}

LRESULT CALLBACK WindowImpl::procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) {
    WindowImpl* window = (WindowImpl*)(GetWindowLongPtrW(handle, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const CREATESTRUCTW& create = *(const CREATESTRUCTW*)(lparam);
        window = (WindowImpl*)(create.lpCreateParams);
        window->handle = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)(window));
    }
    if (window != nullptr) {
        const LRESULT result = window->message(message, wparam, lparam);
        window->platform.queueInputFlush(*window);
        return result;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}

Platform* plt::createWin32Platform(ObjPool& owner) {
    return owner.make<PlatformImpl>(owner);
}
