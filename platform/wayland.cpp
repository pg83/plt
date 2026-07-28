/*
 * Copyright (C) 2026 pg83
 * MIT licensed
 * See the file LICENSE for the full license.
 */

#include "platform.h"

#include "pointer_grab.h"

#include <std/alg/minmax.h>
#include <std/lib/buffer.h>
#include <std/lib/vector.h>
#include <std/mem/obj_pool.h>
#include <std/sym/i_map.h>
#include <std/sys/crt.h>
#include <std/sys/throw.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "primary-selection-unstable-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include "cursor-shape-v1-client-protocol-code.h"
#include "fractional-scale-v1-client-protocol-code.h"
#include "primary-selection-unstable-v1-client-protocol-code.h"
#include "viewporter-client-protocol-code.h"
#include "xdg-activation-v1-client-protocol-code.h"
#include "xdg-decoration-unstable-v1-client-protocol-code.h"
#include "xdg-shell-client-protocol-code.h"

using namespace stl;

namespace plt {
    namespace {
        struct PlatformImpl;
        struct WindowImpl;

        constexpr u32 scaleDenominator = 120;
        const StringView utf8Mime(u8"text/plain;charset=utf-8");
        const StringView plainMime(u8"text/plain");
        const StringView utf8StringMime(u8"UTF8_STRING");

        struct ArmedFD {
            int fd = -1;
            int mode = 0;
        };

        struct Offer {
            struct wl_data_offer* data = nullptr;
            struct zwp_primary_selection_offer_v1* primary = nullptr;
            bool utf8 = false;
            bool plain = false;

            void reset();
            const char* mime() const;
        };

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
            StringView readPrimary() override;
            StringView readClipboard() override;
            void writePrimary(StringView content) override;
            void writeClipboard(StringView content) override;
            void pointerIcon(PointerIcon icon) override;
            RenderContext renderContext() const override;

            void configure();
            void contentScale(u32 numerator);
            void pointerEntered(u32 serial, wl_fixed_t x, wl_fixed_t y);
            void pointerLeft();
            void pointerMoved(wl_fixed_t x, wl_fixed_t y);
            void pointerButton(u32 time, u32 button, u32 state);
            void pointerAxis(u32 axis, wl_fixed_t value);
            void pointerFrame();
            void frameReady(struct wl_callback* callback);
            void updateCursor();
            u32 pixelWidth() const;
            u32 pixelHeight() const;
            u32 logicalForPixel(u32 pixels) const;
            u32 snappedLogical(u32 suggested, u32 unit, u32 base) const;
            void setLogicalSize(u32 width, u32 height, bool notify);
            StringView receive(Offer& offer, bool primary);

            PlatformImpl& platform;
            InputSink* input = nullptr;
            WindowEvents* events = nullptr;
            struct wl_surface* surface = nullptr;
            struct xdg_surface* xdgSurface = nullptr;
            struct xdg_toplevel* toplevel = nullptr;
            struct zxdg_toplevel_decoration_v1* decoration = nullptr;
            struct wp_viewport* viewport = nullptr;
            struct wp_fractional_scale_v1* fractionalScale = nullptr;
            struct wl_callback* frameCallback = nullptr;
            struct xdg_activation_token_v1* activationToken = nullptr;
            Buffer title;
            Buffer clipboardRead;
            u32 logicalWidth = 1;
            u32 logicalHeight = 1;
            u32 pendingWidth = 0;
            u32 pendingHeight = 0;
            u32 scaleNumerator = scaleDenominator;
            u32 minimumWidth = 1;
            u32 minimumHeight = 1;
            u32 resizeUnitWidth = 1;
            u32 resizeUnitHeight = 1;
            u32 resizeBaseWidth = 0;
            u32 resizeBaseHeight = 0;
            i32 pointerX = 0;
            i32 pointerY = 0;
            double scrollX = 0;
            double scrollY = 0;
            PointerIcon cursor = PointerIcon::Text;
            bool shown = false;
            bool configured = false;
            bool closeRequested = false;
            bool focused = false;
            bool maximized = false;
            bool fullscreen = false;
            bool tiled = false;
            bool pendingFocused = false;
            bool pendingMaximized = false;
            bool pendingFullscreen = false;
            bool pendingTiled = false;
        };

        struct PlatformImpl final: public Platform {
            PlatformImpl(ObjPool& owner, PlatformEvents& events);
            ~PlatformImpl();

            Window* createWindow(ObjPool& owner, const WindowOptions& options) override;
            void arm(int fd, int mode) override;
            void disarm(int fd) override;
            void timeout(u64 microseconds) override;
            void deadline(u64 monotonicMicroseconds) override;
            void run() override;
            void stop() override;

            void bindRegistry(u32 name, const char* interface, u32 version);
            void seatCapabilities(u32 capabilities);
            void createSelectionDevices();
            void dispatch();
            void wait();
            void dispatchReady();
            void dispatchTimeouts();
            int timeoutMilliseconds() const;
            void keyboardKey(u32 serial, u32 time, u32 key, u32 state, bool repeated = false);
            void repeat();
            void stopRepeat();
            u16 modifiers() const;
            InputKey inputKey(xkb_keysym_t symbol) const;
            u32 baseCodepoint(xkb_keycode_t key) const;
            void applyClipboardSelection();
            void applyPrimarySelection();
            void setClipboard(StringView content);
            void setPrimary(StringView content);
            void setCursor(WindowImpl& window);
            void activate(WindowImpl& window);
            void writeSelection(int fd, StringView content);
            StringView readSelection(int fd, Buffer& destination);
            static short nativeEvents(int mode);
            static int platformEvents(short events);

            ObjPool& owner;
            PlatformEvents& events;
            IntMap<ArmedFD> armed;
            Vector<struct pollfd> pollFDs;
            Vector<FDReady> ready;
            struct wl_display* display = nullptr;
            struct wl_registry* registry = nullptr;
            struct wl_compositor* compositor = nullptr;
            struct xdg_wm_base* wmBase = nullptr;
            struct wl_seat* seat = nullptr;
            struct wl_keyboard* keyboard = nullptr;
            struct wl_pointer* pointer = nullptr;
            struct wl_data_device_manager* dataDeviceManager = nullptr;
            struct wl_data_device* dataDevice = nullptr;
            struct wl_data_source* clipboardSource = nullptr;
            struct zwp_primary_selection_device_manager_v1* primaryManager = nullptr;
            struct zwp_primary_selection_device_v1* primaryDevice = nullptr;
            struct zwp_primary_selection_source_v1* primarySource = nullptr;
            struct wp_viewporter* viewporter = nullptr;
            struct wp_fractional_scale_manager_v1* fractionalScaleManager = nullptr;
            struct zxdg_decoration_manager_v1* decorationManager = nullptr;
            struct xdg_activation_v1* activation = nullptr;
            struct wp_cursor_shape_manager_v1* cursorShapeManager = nullptr;
            struct wp_cursor_shape_device_v1* cursorShapeDevice = nullptr;
            struct wl_output* output = nullptr;
            struct xkb_context* xkbContext = nullptr;
            struct xkb_keymap* keymap = nullptr;
            struct xkb_state* xkbState = nullptr;
            WindowImpl* keyboardFocus = nullptr;
            PointerGrab pointerGrab;
            WindowImpl* repeatWindow = nullptr;
            u32 repeatKeycode = 0;
            u32 repeatSerial = 0;
            u32 repeatTime = 0;
            u32 repeatRate = 0;
            u32 repeatDelay = 0;
            u64 repeatDeadline = 0;
            u64 minDeadline = 0;
            u32 latestSerial = 0;
            u32 outputWidth = 0;
            u32 outputHeight = 0;
            i32 outputScale = 1;
            Offer pendingClipboardOffer;
            Offer clipboardOffer;
            Offer pendingPrimaryOffer;
            Offer primaryOffer;
            Buffer clipboardContent;
            Buffer primaryContent;
            bool clipboardPending = false;
            bool primaryPending = false;
            bool timeoutReady = false;
            bool running = false;
        };

        bool textMime(const char* mime) {
            const StringView value(mime);
            return value == utf8Mime || value == plainMime || value == utf8StringMime;
        }

        [[noreturn]]
        void fail(StringView message) {
            Errno(errno == 0 ? EINVAL : errno).raise(message);
        }

        void offerMime(Offer& offer, const char* mime) {
            if (!textMime(mime)) {
                return;
            }
            if (StringView(mime) == utf8Mime || StringView(mime) == utf8StringMime) {
                offer.utf8 = true;
            } else {
                offer.plain = true;
            }
        }

        void dataOfferOffer(void* data, struct wl_data_offer*, const char* mime) {
            offerMime(*(Offer*)(data), mime);
        }

        const struct wl_data_offer_listener dataOfferListener{
            .offer = dataOfferOffer,
            .source_actions = [](void*, struct wl_data_offer*, u32) {},
            .action = [](void*, struct wl_data_offer*, u32) {},
        };

        void primaryOfferOffer(void* data, struct zwp_primary_selection_offer_v1*, const char* mime) {
            offerMime(*(Offer*)(data), mime);
        }

        const struct zwp_primary_selection_offer_v1_listener primaryOfferListener{
            .offer = primaryOfferOffer,
        };

        void dataSourceTarget(void*, struct wl_data_source*, const char*) {
        }

        void dataSourceSend(void* data, struct wl_data_source*, const char*, int fd) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.writeSelection(fd, StringView(platform.clipboardContent));
        }

        void dataSourceCancelled(void* data, struct wl_data_source* source) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            if (platform.clipboardSource == source) {
                platform.clipboardSource = nullptr;
            }
            wl_data_source_destroy(source);
        }

        const struct wl_data_source_listener dataSourceListener{
            .target = dataSourceTarget,
            .send = dataSourceSend,
            .cancelled = dataSourceCancelled,
            .dnd_drop_performed = [](void*, struct wl_data_source*) {},
            .dnd_finished = [](void*, struct wl_data_source*) {},
            .action = [](void*, struct wl_data_source*, u32) {},
        };

        void primarySourceSend(void* data, struct zwp_primary_selection_source_v1*, const char*, int fd) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.writeSelection(fd, StringView(platform.primaryContent));
        }

        void primarySourceCancelled(void* data, struct zwp_primary_selection_source_v1* source) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            if (platform.primarySource == source) {
                platform.primarySource = nullptr;
            }
            zwp_primary_selection_source_v1_destroy(source);
        }

        const struct zwp_primary_selection_source_v1_listener primarySourceListener{
            .send = primarySourceSend,
            .cancelled = primarySourceCancelled,
        };

        void dataDeviceDataOffer(void* data, struct wl_data_device*, struct wl_data_offer* proxy) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.pendingClipboardOffer.reset();
            platform.pendingClipboardOffer.data = proxy;
            wl_data_offer_add_listener(proxy, &dataOfferListener, &platform.pendingClipboardOffer);
        }

        void dataDeviceSelection(void* data, struct wl_data_device*, struct wl_data_offer* proxy) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.clipboardOffer.reset();
            if (proxy == nullptr) {
                return;
            }
            if (platform.pendingClipboardOffer.data == proxy) {
                platform.clipboardOffer = platform.pendingClipboardOffer;
                platform.pendingClipboardOffer = {};
            } else {
                platform.clipboardOffer.data = proxy;
                wl_data_offer_add_listener(proxy, &dataOfferListener, &platform.clipboardOffer);
            }
        }

        const struct wl_data_device_listener dataDeviceListener{
            .data_offer = dataDeviceDataOffer,
            .enter = [](void*, struct wl_data_device*, u32, struct wl_surface*, wl_fixed_t, wl_fixed_t, struct wl_data_offer*) {},
            .leave = [](void*, struct wl_data_device*) {},
            .motion = [](void*, struct wl_data_device*, u32, wl_fixed_t, wl_fixed_t) {},
            .drop = [](void*, struct wl_data_device*) {},
            .selection = dataDeviceSelection,
        };

        void primaryDeviceDataOffer(void* data, struct zwp_primary_selection_device_v1*, struct zwp_primary_selection_offer_v1* proxy) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.pendingPrimaryOffer.reset();
            platform.pendingPrimaryOffer.primary = proxy;
            zwp_primary_selection_offer_v1_add_listener(proxy, &primaryOfferListener, &platform.pendingPrimaryOffer);
        }

        void primaryDeviceSelection(void* data, struct zwp_primary_selection_device_v1*, struct zwp_primary_selection_offer_v1* proxy) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.primaryOffer.reset();
            if (proxy == nullptr) {
                return;
            }
            if (platform.pendingPrimaryOffer.primary == proxy) {
                platform.primaryOffer = platform.pendingPrimaryOffer;
                platform.pendingPrimaryOffer = {};
            } else {
                platform.primaryOffer.primary = proxy;
                zwp_primary_selection_offer_v1_add_listener(proxy, &primaryOfferListener, &platform.primaryOffer);
            }
        }

        const struct zwp_primary_selection_device_v1_listener primaryDeviceListener{
            .data_offer = primaryDeviceDataOffer,
            .selection = primaryDeviceSelection,
        };

        void keyboardKeymap(void* data, struct wl_keyboard*, u32 format, int fd, u32 size) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
                close(fd);
                return;
            }
            void* const mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (mapping == MAP_FAILED) {
                return;
            }
            struct xkb_keymap* const keymap = xkb_keymap_new_from_string(platform.xkbContext, (const char*)(mapping), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(mapping, size);
            if (keymap == nullptr) {
                return;
            }
            struct xkb_state* const state = xkb_state_new(keymap);
            if (state == nullptr) {
                xkb_keymap_unref(keymap);
                return;
            }
            if (platform.xkbState != nullptr) {
                xkb_state_unref(platform.xkbState);
            }
            if (platform.keymap != nullptr) {
                xkb_keymap_unref(platform.keymap);
            }
            platform.keymap = keymap;
            platform.xkbState = state;
        }

        void keyboardEnter(void* data, struct wl_keyboard*, u32 serial, struct wl_surface* surface, struct wl_array*) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.latestSerial = serial;
            platform.keyboardFocus = (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
            if (platform.keyboardFocus != nullptr) {
                platform.keyboardFocus->focused = true;
                if (platform.keyboardFocus->input != nullptr) {
                    platform.keyboardFocus->input->focus(true);
                }
            }
        }

        void keyboardLeave(void* data, struct wl_keyboard*, u32 serial, struct wl_surface* surface) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.latestSerial = serial;
            WindowImpl* const window = (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
            if (window != nullptr) {
                window->focused = false;
                if (window->input != nullptr) {
                    window->input->focus(false);
                }
            }
            if (platform.keyboardFocus == window) {
                platform.keyboardFocus = nullptr;
            }
            platform.stopRepeat();
        }

        void keyboardKey(void* data, struct wl_keyboard*, u32 serial, u32 time, u32 key, u32 state) {
            ((PlatformImpl*)(data))->keyboardKey(serial, time, key, state);
        }

        void keyboardModifiers(void* data, struct wl_keyboard*, u32 serial, u32 depressed, u32 latched, u32 locked, u32 group) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.latestSerial = serial;
            if (platform.xkbState != nullptr) {
                xkb_state_update_mask(platform.xkbState, depressed, latched, locked, 0, 0, group);
            }
        }

        void keyboardRepeatInfo(void* data, struct wl_keyboard*, i32 rate, i32 delay) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.repeatRate = rate > 0 ? (u32)(rate) : 0;
            platform.repeatDelay = delay > 0 ? (u32)(delay) : 0;
        }

        const struct wl_keyboard_listener keyboardListener{
            .keymap = keyboardKeymap,
            .enter = keyboardEnter,
            .leave = keyboardLeave,
            .key = keyboardKey,
            .modifiers = keyboardModifiers,
            .repeat_info = keyboardRepeatInfo,
        };

        void pointerEnter(void* data, struct wl_pointer*, u32 serial, struct wl_surface* surface, wl_fixed_t x, wl_fixed_t y) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.latestSerial = serial;
            WindowImpl* const window = (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
            platform.pointerGrab.enter(window);
            if (window != nullptr) {
                window->pointerEntered(serial, x, y);
            }
        }

        void pointerLeave(void* data, struct wl_pointer*, u32 serial, struct wl_surface* surface) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.latestSerial = serial;
            WindowImpl* const window = (WindowImpl*)(wl_proxy_get_user_data((struct wl_proxy*)(surface)));
            if (window != nullptr) {
                window->pointerLeft();
            }
            platform.pointerGrab.leave(window);
        }

        void pointerMotion(void* data, struct wl_pointer*, u32, wl_fixed_t x, wl_fixed_t y) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.eventTarget());
            if (window != nullptr) {
                window->pointerMoved(x, y);
            }
        }

        void pointerButton(void* data, struct wl_pointer*, u32 serial, u32 time, u32 button, u32 state) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            platform.latestSerial = serial;
            WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.buttonTarget(state == WL_POINTER_BUTTON_STATE_PRESSED));
            if (window != nullptr) {
                window->pointerButton(time, button, state);
            }
        }

        void pointerAxis(void* data, struct wl_pointer*, u32, u32 axis, wl_fixed_t value) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.eventTarget());
            if (window != nullptr) {
                window->pointerAxis(axis, value);
            }
        }

        void pointerFrame(void* data, struct wl_pointer*) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            WindowImpl* const window = (WindowImpl*)(platform.pointerGrab.eventTarget());
            if (window != nullptr) {
                window->pointerFrame();
            }
        }

        const struct wl_pointer_listener pointerListener{
            .enter = pointerEnter,
            .leave = pointerLeave,
            .motion = pointerMotion,
            .button = pointerButton,
            .axis = pointerAxis,
            .frame = pointerFrame,
            .axis_source = [](void*, struct wl_pointer*, u32) {},
            .axis_stop = [](void*, struct wl_pointer*, u32, u32) {},
            .axis_discrete = [](void*, struct wl_pointer*, u32, i32) {},
            .axis_value120 = [](void*, struct wl_pointer*, u32, i32) {},
            .axis_relative_direction = [](void*, struct wl_pointer*, u32, u32) {},
            .warp = [](void* data, struct wl_pointer*, wl_fixed_t x, wl_fixed_t y) {
            pointerMotion(data, nullptr, 0, x, y);
        },
        };

        void seatCapabilities(void* data, struct wl_seat*, u32 capabilities) {
            ((PlatformImpl*)(data))->seatCapabilities(capabilities);
        }

        const struct wl_seat_listener seatListener{
            .capabilities = seatCapabilities,
            .name = [](void*, struct wl_seat*, const char*) {},
        };

        void outputMode(void* data, struct wl_output*, u32 flags, i32 width, i32 height, i32) {
            PlatformImpl& platform = *(PlatformImpl*)(data);
            if (flags & WL_OUTPUT_MODE_CURRENT) {
                platform.outputWidth = width > 0 ? (u32)(width) : 0;
                platform.outputHeight = height > 0 ? (u32)(height) : 0;
            }
        }

        void outputScale(void* data, struct wl_output*, i32 scale) {
            ((PlatformImpl*)(data))->outputScale = max(1, scale);
        }

        const struct wl_output_listener outputListener{
            .geometry = [](void*, struct wl_output*, i32, i32, i32, i32, i32, const char*, const char*, i32) {},
            .mode = outputMode,
            .done = [](void*, struct wl_output*) {},
            .scale = outputScale,
            .name = [](void*, struct wl_output*, const char*) {},
            .description = [](void*, struct wl_output*, const char*) {},
        };

        void registryGlobal(void* data, struct wl_registry*, u32 name, const char* interface, u32 version) {
            ((PlatformImpl*)(data))->bindRegistry(name, interface, version);
        }

        const struct wl_registry_listener registryListener{
            .global = registryGlobal,
            .global_remove = [](void*, struct wl_registry*, u32) {},
        };

        const struct xdg_wm_base_listener wmBaseListener{
            .ping = [](void*, struct xdg_wm_base* wmBase, u32 serial) {
            xdg_wm_base_pong(wmBase, serial);
        },
        };

        void surfaceEnter(void* data, struct wl_surface*, struct wl_output*) {
            WindowImpl& window = *(WindowImpl*)(data);
            if (window.fractionalScale == nullptr) {
                window.contentScale((u32)(window.platform.outputScale) * scaleDenominator);
            }
        }

        const struct wl_surface_listener surfaceListener{
            .enter = surfaceEnter,
            .leave = [](void*, struct wl_surface*, struct wl_output*) {},
            .preferred_buffer_scale =
                [](void* data, struct wl_surface*, i32 scale) {
            WindowImpl& window = *(WindowImpl*)(data);
            if (window.fractionalScale == nullptr) {
                window.contentScale((u32)(max(1, scale)) * scaleDenominator);
            }
        },
            .preferred_buffer_transform = [](void*, struct wl_surface*, u32) {},
        };

        void toplevelConfigure(void* data, struct xdg_toplevel*, i32 width, i32 height, struct wl_array* states) {
            WindowImpl& window = *(WindowImpl*)(data);
            window.pendingFocused = false;
            window.pendingMaximized = false;
            window.pendingFullscreen = false;
            window.pendingTiled = false;
            const u32* state = (const u32*)(states->data);
            const u32* const stateEnd = state + states->size / sizeof(*state);
            for (; state != stateEnd; ++state) {
                switch (*state) {
                    case XDG_TOPLEVEL_STATE_ACTIVATED:
                        window.pendingFocused = true;
                        break;
                    case XDG_TOPLEVEL_STATE_MAXIMIZED:
                        window.pendingMaximized = true;
                        break;
                    case XDG_TOPLEVEL_STATE_FULLSCREEN:
                        window.pendingFullscreen = true;
                        break;
                    case XDG_TOPLEVEL_STATE_TILED_LEFT:
                    case XDG_TOPLEVEL_STATE_TILED_RIGHT:
                    case XDG_TOPLEVEL_STATE_TILED_TOP:
                    case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
                        window.pendingTiled = true;
                        break;
                    default:
                        break;
                }
            }
            window.pendingWidth = width > 0 ? (u32)(width) : window.logicalWidth;
            window.pendingHeight = height > 0 ? (u32)(height) : window.logicalHeight;
        }

        const struct xdg_toplevel_listener toplevelListener{
            .configure = toplevelConfigure,
            .close =
                [](void* data, struct xdg_toplevel*) {
            WindowImpl& window = *(WindowImpl*)(data);
            window.closeRequested = true;
            if (window.events != nullptr) {
                window.events->close();
            }
        },
            .configure_bounds = [](void*, struct xdg_toplevel*, i32, i32) {},
            .wm_capabilities = [](void*, struct xdg_toplevel*, struct wl_array*) {},
        };

        const struct xdg_surface_listener xdgSurfaceListener{
            .configure = [](void* data, struct xdg_surface* surface, u32 serial) {
            xdg_surface_ack_configure(surface, serial);
            ((WindowImpl*)(data))->configure();
        },
        };

        const struct wp_fractional_scale_v1_listener fractionalScaleListener{
            .preferred_scale = [](void* data, struct wp_fractional_scale_v1*, u32 numerator) {
            ((WindowImpl*)(data))->contentScale(numerator);
        },
        };

        const struct wl_callback_listener frameListener{
            .done = [](void* data, struct wl_callback* callback, u32) {
            ((WindowImpl*)(data))->frameReady(callback);
        },
        };

        const struct xdg_activation_token_v1_listener activationTokenListener{
            .done = [](void* data, struct xdg_activation_token_v1* token, const char* value) {
            WindowImpl& window = *(WindowImpl*)(data);
            if (window.activationToken != token) {
                xdg_activation_token_v1_destroy(token);
                return;
            }
            if (window.platform.activation != nullptr) {
                xdg_activation_v1_activate(window.platform.activation, value, window.surface);
            }
            xdg_activation_token_v1_destroy(token);
            window.activationToken = nullptr;
        },
        };
    }

    void Offer::reset() {
        if (data != nullptr) {
            wl_data_offer_destroy(data);
        }
        if (primary != nullptr) {
            zwp_primary_selection_offer_v1_destroy(primary);
        }
        *this = {};
    }

    const char* Offer::mime() const {
        if (utf8) {
            return "text/plain;charset=utf-8";
        }
        return plain ? "text/plain" : nullptr;
    }

    PlatformImpl::PlatformImpl(ObjPool& owner_, PlatformEvents& events_)
        : owner(owner_)
        , events(events_)
        , armed(ObjPool::create(&owner))
    {
        display = wl_display_connect(nullptr);
        if (display == nullptr) {
            fail(u8"wl_display_connect failed");
        }
        xkbContext = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (xkbContext == nullptr) {
            fail(u8"xkb_context_new failed");
        }
        registry = wl_display_get_registry(display);
        wl_registry_add_listener(registry, &registryListener, this);
        if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) {
            fail(u8"Wayland registry roundtrip failed");
        }
        if (compositor == nullptr || wmBase == nullptr || seat == nullptr) {
            fail(u8"Wayland compositor lacks required globals");
        }
        createSelectionDevices();
    }

    PlatformImpl::~PlatformImpl() {
        stopRepeat();
        pendingClipboardOffer.reset();
        clipboardOffer.reset();
        pendingPrimaryOffer.reset();
        primaryOffer.reset();
        if (clipboardSource != nullptr) {
            wl_data_source_destroy(clipboardSource);
        }
        if (primarySource != nullptr) {
            zwp_primary_selection_source_v1_destroy(primarySource);
        }
        if (cursorShapeDevice != nullptr) {
            wp_cursor_shape_device_v1_destroy(cursorShapeDevice);
        }
        if (pointer != nullptr) {
            wl_pointer_release(pointer);
        }
        if (keyboard != nullptr) {
            wl_keyboard_release(keyboard);
        }
        if (dataDevice != nullptr) {
            wl_data_device_release(dataDevice);
        }
        if (primaryDevice != nullptr) {
            zwp_primary_selection_device_v1_destroy(primaryDevice);
        }
        if (output != nullptr) {
            wl_output_release(output);
        }
        if (seat != nullptr) {
            wl_seat_release(seat);
        }
        if (cursorShapeManager != nullptr) {
            wp_cursor_shape_manager_v1_destroy(cursorShapeManager);
        }
        if (activation != nullptr) {
            xdg_activation_v1_destroy(activation);
        }
        if (decorationManager != nullptr) {
            zxdg_decoration_manager_v1_destroy(decorationManager);
        }
        if (fractionalScaleManager != nullptr) {
            wp_fractional_scale_manager_v1_destroy(fractionalScaleManager);
        }
        if (viewporter != nullptr) {
            wp_viewporter_destroy(viewporter);
        }
        if (primaryManager != nullptr) {
            zwp_primary_selection_device_manager_v1_destroy(primaryManager);
        }
        if (dataDeviceManager != nullptr) {
            wl_data_device_manager_destroy(dataDeviceManager);
        }
        if (wmBase != nullptr) {
            xdg_wm_base_destroy(wmBase);
        }
        if (compositor != nullptr) {
            wl_compositor_destroy(compositor);
        }
        if (registry != nullptr) {
            wl_registry_destroy(registry);
        }
        if (xkbState != nullptr) {
            xkb_state_unref(xkbState);
        }
        if (keymap != nullptr) {
            xkb_keymap_unref(keymap);
        }
        if (xkbContext != nullptr) {
            xkb_context_unref(xkbContext);
        }
        if (display != nullptr) {
            wl_display_disconnect(display);
        }
    }

    Window* PlatformImpl::createWindow(ObjPool& windowOwner, const WindowOptions& options) {
        return windowOwner.make<WindowImpl>(*this, options);
    }

    void PlatformImpl::bindRegistry(u32 name, const char* interface, u32 version) {
        if (StringView(interface) == StringView(wl_compositor_interface.name)) {
            compositor = (struct wl_compositor*)(wl_registry_bind(registry, name, &wl_compositor_interface, min(version, 6u)));
        } else if (StringView(interface) == StringView(xdg_wm_base_interface.name)) {
            wmBase = (struct xdg_wm_base*)(wl_registry_bind(registry, name, &xdg_wm_base_interface, min(version, 6u)));
            xdg_wm_base_add_listener(wmBase, &wmBaseListener, this);
        } else if (StringView(interface) == StringView(wl_seat_interface.name)) {
            seat = (struct wl_seat*)(wl_registry_bind(registry, name, &wl_seat_interface, min(version, 8u)));
            wl_seat_add_listener(seat, &seatListener, this);
        } else if (StringView(interface) == StringView(wl_data_device_manager_interface.name)) {
            dataDeviceManager = (struct wl_data_device_manager*)(wl_registry_bind(registry, name, &wl_data_device_manager_interface, min(version, 3u)));
        } else if (StringView(interface) == StringView(zwp_primary_selection_device_manager_v1_interface.name)) {
            primaryManager = (struct zwp_primary_selection_device_manager_v1*)(wl_registry_bind(registry, name, &zwp_primary_selection_device_manager_v1_interface, 1));
        } else if (StringView(interface) == StringView(wp_viewporter_interface.name)) {
            viewporter = (struct wp_viewporter*)(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
        } else if (StringView(interface) == StringView(wp_fractional_scale_manager_v1_interface.name)) {
            fractionalScaleManager = (struct wp_fractional_scale_manager_v1*)(wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1));
        } else if (StringView(interface) == StringView(zxdg_decoration_manager_v1_interface.name)) {
            decorationManager = (struct zxdg_decoration_manager_v1*)(wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1));
        } else if (StringView(interface) == StringView(xdg_activation_v1_interface.name)) {
            activation = (struct xdg_activation_v1*)(wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1));
        } else if (StringView(interface) == StringView(wp_cursor_shape_manager_v1_interface.name)) {
            cursorShapeManager = (struct wp_cursor_shape_manager_v1*)(wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1));
        } else if (StringView(interface) == StringView(wl_output_interface.name) && output == nullptr) {
            output = (struct wl_output*)(wl_registry_bind(registry, name, &wl_output_interface, min(version, 4u)));
            wl_output_add_listener(output, &outputListener, this);
        }
    }

    void PlatformImpl::seatCapabilities(u32 capabilities) {
        if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard == nullptr) {
            keyboard = wl_seat_get_keyboard(seat);
            wl_keyboard_add_listener(keyboard, &keyboardListener, this);
        } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && keyboard != nullptr) {
            wl_keyboard_release(keyboard);
            keyboard = nullptr;
            keyboardFocus = nullptr;
            stopRepeat();
        }
        if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && pointer == nullptr) {
            pointer = wl_seat_get_pointer(seat);
            wl_pointer_add_listener(pointer, &pointerListener, this);
            if (cursorShapeManager != nullptr) {
                cursorShapeDevice = wp_cursor_shape_manager_v1_get_pointer(cursorShapeManager, pointer);
            }
        } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && pointer != nullptr) {
            if (cursorShapeDevice != nullptr) {
                wp_cursor_shape_device_v1_destroy(cursorShapeDevice);
                cursorShapeDevice = nullptr;
            }
            wl_pointer_release(pointer);
            pointer = nullptr;
            pointerGrab.reset();
        }
        createSelectionDevices();
    }

    void PlatformImpl::createSelectionDevices() {
        if (seat == nullptr) {
            return;
        }
        if (dataDeviceManager != nullptr && dataDevice == nullptr) {
            dataDevice = wl_data_device_manager_get_data_device(dataDeviceManager, seat);
            wl_data_device_add_listener(dataDevice, &dataDeviceListener, this);
        }
        if (primaryManager != nullptr && primaryDevice == nullptr) {
            primaryDevice = zwp_primary_selection_device_manager_v1_get_device(primaryManager, seat);
            zwp_primary_selection_device_v1_add_listener(primaryDevice, &primaryDeviceListener, this);
        }
    }

    void PlatformImpl::arm(int fd, int mode) {
        armed[fd] = {fd, mode};
    }

    void PlatformImpl::disarm(int fd) {
        armed.erase(fd);
    }

    void PlatformImpl::timeout(u64 microseconds) {
        deadline(monotonicNowUs() + microseconds);
    }

    void PlatformImpl::deadline(u64 value) {
        if (value == 0) {
            value = monotonicNowUs();
        }
        if (minDeadline == 0 || value < minDeadline) {
            minDeadline = value;
        }
    }

    short PlatformImpl::nativeEvents(int mode) {
        short result = 0;
        if (mode & PollRead) {
            result |= POLLIN;
        }
        if (mode & PollWrite) {
            result |= POLLOUT;
        }
        return result;
    }

    int PlatformImpl::platformEvents(short events) {
        int result = 0;
        if (events & POLLIN) {
            result |= PollRead;
        }
        if (events & POLLOUT) {
            result |= PollWrite;
        }
        if (events & (POLLERR | POLLNVAL)) {
            result |= PollError;
        }
        if (events & POLLHUP) {
            result |= PollHangup;
        }
        return result;
    }

    int PlatformImpl::timeoutMilliseconds() const {
        u64 next = minDeadline;
        if (repeatDeadline != 0 && (next == 0 || repeatDeadline < next)) {
            next = repeatDeadline;
        }
        if (next == 0) {
            return -1;
        }
        const u64 now = monotonicNowUs();
        if (next <= now) {
            return 0;
        }
        const u64 milliseconds = (next - now + 999) / 1000;
        return milliseconds > INT_MAX ? INT_MAX : (int)(milliseconds);
    }

    void PlatformImpl::wait() {
        while (wl_display_prepare_read(display) != 0) {
            if (wl_display_dispatch_pending(display) < 0) {
                running = false;
                return;
            }
        }
        wl_display_flush(display);

        pollFDs.clear();
        pollFDs.pushBack({
            .fd = wl_display_get_fd(display),
            .events = POLLIN,
            .revents = 0,
        });
        armed.visit([this](const ArmedFD& fd) {
            pollFDs.pushBack({
                .fd = fd.fd,
                .events = nativeEvents(fd.mode),
                .revents = 0,
            });
        });

        int result;
        do {
            result = ::poll(pollFDs.mutData(), pollFDs.length(), timeoutMilliseconds());
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            wl_display_cancel_read(display);
            fail(u8"poll failed");
        }

        const short displayEvents = pollFDs[0].revents;
        if (displayEvents & POLLIN) {
            if (wl_display_read_events(display) < 0) {
                running = false;
                return;
            }
        } else {
            wl_display_cancel_read(display);
        }
        if (displayEvents & (POLLERR | POLLHUP | POLLNVAL)) {
            running = false;
        }

        ready.clear();
        for (size_t index = 1; index != pollFDs.length(); ++index) {
            const struct pollfd& source = pollFDs[index];
            if (source.revents != 0) {
                ready.pushBack({
                    .fd = source.fd,
                    .what = platformEvents(source.revents),
                });
            }
        }
    }

    void PlatformImpl::dispatchReady() {
        for (const FDReady& event : ready) {
            events.fdReady(event);
        }
        ready.clear();
    }

    void PlatformImpl::dispatchTimeouts() {
        const u64 now = monotonicNowUs();
        if (repeatDeadline != 0 && now >= repeatDeadline) {
            repeat();
        }
        if (minDeadline != 0 && now >= minDeadline) {
            minDeadline = 0;
            events.timeout();
        }
    }

    void PlatformImpl::dispatch() {
        if (wl_display_dispatch_pending(display) < 0) {
            running = false;
            return;
        }
        dispatchReady();
        dispatchTimeouts();
        if (keyboardFocus != nullptr && keyboardFocus->input != nullptr) {
            keyboardFocus->input->flush();
        }
        WindowImpl* const pointerTarget = (WindowImpl*)(pointerGrab.eventTarget());
        if (pointerTarget != nullptr && pointerTarget != keyboardFocus && pointerTarget->input != nullptr) {
            pointerTarget->input->flush();
        }
        events.check();
    }

    void PlatformImpl::run() {
        running = true;
        while (running) {
            wait();
            dispatch();
        }
    }

    void PlatformImpl::stop() {
        running = false;
    }

    u16 PlatformImpl::modifiers() const {
        if (xkbState == nullptr) {
            return 0;
        }
        auto active = [this](const char* name) {
            return xkb_state_mod_name_is_active(xkbState, name, XKB_STATE_MODS_EFFECTIVE) > 0;
        };
        u16 result = 0;
        if (active(XKB_MOD_NAME_SHIFT)) {
            result |= InputShift;
        }
        if (active(XKB_MOD_NAME_CTRL)) {
            result |= InputControl;
        }
        if (active(XKB_MOD_NAME_ALT)) {
            result |= InputAlt;
        }
        if (active(XKB_MOD_NAME_LOGO)) {
            result |= InputSuper;
        }
        if (active(XKB_MOD_NAME_CAPS)) {
            result |= InputCapsLock;
        }
        if (active(XKB_MOD_NAME_NUM)) {
            result |= InputNumLock;
        }
        if (active("Mod5")) {
            result |= InputAltGraph;
        }
        return result;
    }

    InputKey PlatformImpl::inputKey(xkb_keysym_t symbol) const {
        if (symbol >= XKB_KEY_a && symbol <= XKB_KEY_z) {
            return InputKey::Printable;
        }
        if (symbol >= XKB_KEY_A && symbol <= XKB_KEY_Z) {
            return InputKey::Printable;
        }
        if ((symbol >= XKB_KEY_0 && symbol <= XKB_KEY_9) || (symbol >= XKB_KEY_space && symbol <= XKB_KEY_asciitilde)) {
            return InputKey::Printable;
        }
        switch (symbol) {
            case XKB_KEY_Escape:
                return InputKey::Escape;
            case XKB_KEY_Return:
                return InputKey::Enter;
            case XKB_KEY_BackSpace:
                return InputKey::Backspace;
            case XKB_KEY_Tab:
            case XKB_KEY_ISO_Left_Tab:
                return InputKey::Tab;
            case XKB_KEY_Insert:
                return InputKey::Insert;
            case XKB_KEY_Delete:
                return InputKey::Delete;
            case XKB_KEY_Home:
                return InputKey::Home;
            case XKB_KEY_End:
                return InputKey::End;
            case XKB_KEY_Up:
                return InputKey::Up;
            case XKB_KEY_Down:
                return InputKey::Down;
            case XKB_KEY_Left:
                return InputKey::Left;
            case XKB_KEY_Right:
                return InputKey::Right;
            case XKB_KEY_Page_Up:
                return InputKey::PageUp;
            case XKB_KEY_Page_Down:
                return InputKey::PageDown;
            case XKB_KEY_F1:
            case XKB_KEY_F2:
            case XKB_KEY_F3:
            case XKB_KEY_F4:
            case XKB_KEY_F5:
            case XKB_KEY_F6:
            case XKB_KEY_F7:
            case XKB_KEY_F8:
            case XKB_KEY_F9:
            case XKB_KEY_F10:
            case XKB_KEY_F11:
            case XKB_KEY_F12:
            case XKB_KEY_F13:
            case XKB_KEY_F14:
            case XKB_KEY_F15:
            case XKB_KEY_F16:
            case XKB_KEY_F17:
            case XKB_KEY_F18:
            case XKB_KEY_F19:
            case XKB_KEY_F20:
                return (InputKey)((u8)(InputKey::F1) + symbol - XKB_KEY_F1);
            case XKB_KEY_KP_0:
            case XKB_KEY_KP_1:
            case XKB_KEY_KP_2:
            case XKB_KEY_KP_3:
            case XKB_KEY_KP_4:
            case XKB_KEY_KP_5:
            case XKB_KEY_KP_6:
            case XKB_KEY_KP_7:
            case XKB_KEY_KP_8:
            case XKB_KEY_KP_9:
                return (InputKey)((u8)(InputKey::Keypad0) + symbol - XKB_KEY_KP_0);
            case XKB_KEY_KP_Decimal:
                return InputKey::KeypadDecimal;
            case XKB_KEY_KP_Divide:
                return InputKey::KeypadDivide;
            case XKB_KEY_KP_Multiply:
                return InputKey::KeypadMultiply;
            case XKB_KEY_KP_Subtract:
                return InputKey::KeypadSubtract;
            case XKB_KEY_KP_Add:
                return InputKey::KeypadAdd;
            case XKB_KEY_KP_Enter:
                return InputKey::KeypadEnter;
            case XKB_KEY_KP_Equal:
                return InputKey::KeypadEqual;
            case XKB_KEY_Caps_Lock:
                return InputKey::CapsLock;
            case XKB_KEY_Scroll_Lock:
                return InputKey::ScrollLock;
            case XKB_KEY_Num_Lock:
                return InputKey::NumLock;
            case XKB_KEY_Print:
                return InputKey::PrintScreen;
            case XKB_KEY_Pause:
                return InputKey::Pause;
            case XKB_KEY_Menu:
                return InputKey::Menu;
            case XKB_KEY_Shift_L:
                return InputKey::LeftShift;
            case XKB_KEY_Control_L:
                return InputKey::LeftControl;
            case XKB_KEY_Alt_L:
                return InputKey::LeftAlt;
            case XKB_KEY_Super_L:
                return InputKey::LeftSuper;
            case XKB_KEY_Shift_R:
                return InputKey::RightShift;
            case XKB_KEY_Control_R:
                return InputKey::RightControl;
            case XKB_KEY_Alt_R:
            case XKB_KEY_ISO_Level3_Shift:
                return InputKey::RightAlt;
            case XKB_KEY_Super_R:
                return InputKey::RightSuper;
            default:
                return xkb_keysym_to_utf32(symbol) != 0 ? InputKey::Printable : InputKey::Unknown;
        }
    }

    u32 PlatformImpl::baseCodepoint(xkb_keycode_t key) const {
        if (keymap == nullptr) {
            return 0;
        }
        const xkb_keysym_t* symbols = nullptr;
        if (xkb_keymap_key_get_syms_by_level(keymap, key, 0, 0, &symbols) <= 0) {
            return 0;
        }
        return xkb_keysym_to_utf32(symbols[0]);
    }

    void PlatformImpl::keyboardKey(u32 serial, u32 time, u32 key, u32 state, bool repeated) {
        latestSerial = serial;
        if (keyboardFocus == nullptr || keyboardFocus->input == nullptr || xkbState == nullptr) {
            return;
        }
        const xkb_keycode_t keycode = key + 8;
        const xkb_keysym_t symbol = xkb_state_key_get_one_sym(xkbState, keycode);
        const InputAction action = repeated ? InputAction::Repeat : (state == WL_KEYBOARD_KEY_STATE_PRESSED ? InputAction::Press : InputAction::Release);
        const u32 codepoint = xkb_state_key_get_utf32(xkbState, keycode);
        keyboardFocus->input->key({
            .key = inputKey(symbol),
            .action = action,
            .modifiers = modifiers(),
            .layoutCodepoint = codepoint,
            .baseCodepoint = baseCodepoint(keycode),
        });
        if (action != InputAction::Release && codepoint >= 0x20 && codepoint != 0x7f && !(modifiers() & (InputControl | InputSuper))) {
            keyboardFocus->input->text({
                .codepoint = codepoint,
                .modifiers = modifiers(),
            });
        }

        if (!repeated && state == WL_KEYBOARD_KEY_STATE_PRESSED && repeatRate != 0 && keymap != nullptr && xkb_keymap_key_repeats(keymap, keycode)) {
            repeatWindow = keyboardFocus;
            repeatKeycode = key;
            repeatSerial = serial;
            repeatTime = time;
            repeatDeadline = monotonicNowUs() + (u64)(repeatDelay) * 1000;
        } else if (!repeated && state == WL_KEYBOARD_KEY_STATE_RELEASED && repeatWindow == keyboardFocus && repeatKeycode == key) {
            stopRepeat();
        }
    }

    void PlatformImpl::repeat() {
        if (repeatWindow == nullptr || repeatRate == 0) {
            stopRepeat();
            return;
        }
        keyboardFocus = repeatWindow;
        keyboardKey(repeatSerial, repeatTime, repeatKeycode, WL_KEYBOARD_KEY_STATE_PRESSED, true);
        repeatDeadline = monotonicNowUs() + 1'000'000 / repeatRate;
    }

    void PlatformImpl::stopRepeat() {
        repeatWindow = nullptr;
        repeatKeycode = 0;
        repeatDeadline = 0;
    }

    void PlatformImpl::writeSelection(int fd, StringView content) {
        const u8* cursor = content.begin();
        while (cursor != content.end()) {
            const ssize_t written = write(fd, cursor, content.end() - cursor);
            if (written > 0) {
                cursor += written;
            } else if (written < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fd);
    }

    StringView PlatformImpl::readSelection(int fd, Buffer& destination) {
        destination.reset();
        u8 data[8192];
        for (;;) {
            const ssize_t count = read(fd, data, sizeof(data));
            if (count > 0) {
                destination.append(data, count);
            } else if (count < 0 && errno == EINTR) {
                continue;
            } else {
                break;
            }
        }
        close(fd);
        return StringView(destination);
    }

    void PlatformImpl::applyClipboardSelection() {
        if (!clipboardPending || dataDeviceManager == nullptr || dataDevice == nullptr || latestSerial == 0) {
            return;
        }
        if (clipboardSource != nullptr) {
            wl_data_source_destroy(clipboardSource);
        }
        clipboardSource = wl_data_device_manager_create_data_source(dataDeviceManager);
        wl_data_source_add_listener(clipboardSource, &dataSourceListener, this);
        wl_data_source_offer(clipboardSource, "text/plain;charset=utf-8");
        wl_data_source_offer(clipboardSource, "text/plain");
        wl_data_device_set_selection(dataDevice, clipboardSource, latestSerial);
        clipboardPending = false;
        wl_display_flush(display);
    }

    void PlatformImpl::applyPrimarySelection() {
        if (!primaryPending || primaryManager == nullptr || primaryDevice == nullptr || latestSerial == 0) {
            return;
        }
        if (primarySource != nullptr) {
            zwp_primary_selection_source_v1_destroy(primarySource);
        }
        primarySource = zwp_primary_selection_device_manager_v1_create_source(primaryManager);
        zwp_primary_selection_source_v1_add_listener(primarySource, &primarySourceListener, this);
        zwp_primary_selection_source_v1_offer(primarySource, "text/plain;charset=utf-8");
        zwp_primary_selection_source_v1_offer(primarySource, "text/plain");
        zwp_primary_selection_device_v1_set_selection(primaryDevice, primarySource, latestSerial);
        primaryPending = false;
        wl_display_flush(display);
    }

    void PlatformImpl::setClipboard(StringView content) {
        clipboardContent.reset();
        clipboardContent.append(content.data(), content.length());
        clipboardPending = true;
        applyClipboardSelection();
    }

    void PlatformImpl::setPrimary(StringView content) {
        primaryContent.reset();
        primaryContent.append(content.data(), content.length());
        primaryPending = true;
        applyPrimarySelection();
    }

    void PlatformImpl::setCursor(WindowImpl& window) {
        if (cursorShapeDevice == nullptr || pointerGrab.focusTarget() != &window || latestSerial == 0) {
            return;
        }
        const u32 shape = window.cursor == PointerIcon::Link ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
        wp_cursor_shape_device_v1_set_shape(cursorShapeDevice, latestSerial, shape);
    }

    void PlatformImpl::activate(WindowImpl& window) {
        if (activation == nullptr || window.activationToken != nullptr) {
            return;
        }
        window.activationToken = xdg_activation_v1_get_activation_token(activation);
        xdg_activation_token_v1_add_listener(window.activationToken, &activationTokenListener, &window);
        if (latestSerial != 0 && seat != nullptr) {
            xdg_activation_token_v1_set_serial(window.activationToken, latestSerial, seat);
        }
        xdg_activation_token_v1_set_surface(window.activationToken, window.surface);
        xdg_activation_token_v1_commit(window.activationToken);
    }

    WindowImpl::WindowImpl(PlatformImpl& platform_, const WindowOptions& options)
        : platform(platform_)
        , input(options.input)
        , events(options.events)
        , logicalWidth(max(1u, options.width))
        , logicalHeight(max(1u, options.height))
        , minimumWidth(max(1u, options.minimumWidth))
        , minimumHeight(max(1u, options.minimumHeight))
    {
        surface = wl_compositor_create_surface(platform.compositor);
        if (surface == nullptr) {
            fail(u8"wl_compositor_create_surface failed");
        }
        wl_proxy_set_user_data((struct wl_proxy*)(surface), this);
        wl_surface_add_listener(surface, &surfaceListener, this);
        xdgSurface = xdg_wm_base_get_xdg_surface(platform.wmBase, surface);
        xdg_surface_add_listener(xdgSurface, &xdgSurfaceListener, this);
        toplevel = xdg_surface_get_toplevel(xdgSurface);
        xdg_toplevel_add_listener(toplevel, &toplevelListener, this);

        if (platform.decorationManager != nullptr) {
            decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(platform.decorationManager, toplevel);
            zxdg_toplevel_decoration_v1_set_mode(decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        if (platform.viewporter != nullptr) {
            viewport = wp_viewporter_get_viewport(platform.viewporter, surface);
        }
        if (platform.fractionalScaleManager != nullptr && viewport != nullptr) {
            fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(platform.fractionalScaleManager, surface);
            wp_fractional_scale_v1_add_listener(fractionalScale, &fractionalScaleListener, this);
        }
        if (fractionalScale == nullptr) {
            contentScale((u32)(platform.outputScale) * scaleDenominator);
        }

        Buffer appId(options.appId);
        Buffer initialTitle(options.title);
        xdg_toplevel_set_app_id(toplevel, appId.cStr());
        xdg_toplevel_set_title(toplevel, initialTitle.cStr());
        title = initialTitle;
        setMinimumSize(minimumWidth, minimumHeight);
    }

    WindowImpl::~WindowImpl() {
        if (platform.keyboardFocus == this) {
            platform.keyboardFocus = nullptr;
            platform.stopRepeat();
        }
        platform.pointerGrab.remove(this);
        cancelFrame();
        if (activationToken != nullptr) {
            xdg_activation_token_v1_destroy(activationToken);
        }
        if (fractionalScale != nullptr) {
            wp_fractional_scale_v1_destroy(fractionalScale);
        }
        if (viewport != nullptr) {
            wp_viewport_destroy(viewport);
        }
        if (decoration != nullptr) {
            zxdg_toplevel_decoration_v1_destroy(decoration);
        }
        if (toplevel != nullptr) {
            xdg_toplevel_destroy(toplevel);
        }
        if (xdgSurface != nullptr) {
            xdg_surface_destroy(xdgSurface);
        }
        if (surface != nullptr) {
            wl_surface_destroy(surface);
        }
    }

    u32 WindowImpl::pixelWidth() const {
        return max(1u, (u32)(((u64)(logicalWidth)*scaleNumerator) / scaleDenominator));
    }

    u32 WindowImpl::pixelHeight() const {
        return max(1u, (u32)(((u64)(logicalHeight)*scaleNumerator) / scaleDenominator));
    }

    u32 WindowImpl::logicalForPixel(u32 pixels) const {
        return max(1u, (u32)(((u64)(pixels)*scaleDenominator + scaleNumerator - 1) / scaleNumerator));
    }

    u32 WindowImpl::snappedLogical(u32 suggested, u32 unit, u32 base) const {
        if (unit <= 1 || suggested == 0) {
            return suggested;
        }
        const u32 pixels = max(1u, (u32)(((u64)(suggested)*scaleNumerator) / scaleDenominator));
        if (pixels <= base) {
            return logicalForPixel(base + unit);
        }
        const u32 target = base + ((pixels - base) / unit) * unit;
        for (u32 logical = logicalForPixel(target); logical != 0; --logical) {
            if (((u64)(logical)*scaleNumerator) / scaleDenominator == target) {
                return logical;
            }
            if (logical + 2 < logicalForPixel(target)) {
                break;
            }
        }
        return suggested;
    }

    void WindowImpl::setLogicalSize(u32 width, u32 height, bool notify) {
        width = max(1u, width);
        height = max(1u, height);
        const bool changed = logicalWidth != width || logicalHeight != height;
        logicalWidth = width;
        logicalHeight = height;
        xdg_surface_set_window_geometry(xdgSurface, 0, 0, logicalWidth, logicalHeight);
        if (viewport != nullptr) {
            wp_viewport_set_destination(viewport, logicalWidth, logicalHeight);
        } else {
            wl_surface_set_buffer_scale(surface, max(1, (i32)(scaleNumerator / scaleDenominator)));
        }
        if (notify && (changed || !configured) && events != nullptr) {
            events->resized(info());
        }
    }

    void WindowImpl::configure() {
        focused = pendingFocused;
        maximized = pendingMaximized;
        fullscreen = pendingFullscreen;
        tiled = pendingTiled;
        u32 width = pendingWidth == 0 ? logicalWidth : pendingWidth;
        u32 height = pendingHeight == 0 ? logicalHeight : pendingHeight;
        if (!maximized && !fullscreen && !tiled) {
            width = snappedLogical(width, resizeUnitWidth, resizeBaseWidth);
            height = snappedLogical(height, resizeUnitHeight, resizeBaseHeight);
        }
        const bool first = !configured;
        configured = true;
        setLogicalSize(width, height, true);
        if (first || shown) {
            requestRedraw();
        }
    }

    void WindowImpl::contentScale(u32 numerator) {
        if (numerator == 0 || numerator == scaleNumerator) {
            return;
        }
        scaleNumerator = numerator;
        if (fractionalScale != nullptr) {
            wl_surface_set_buffer_scale(surface, 1);
        }
        setLogicalSize(logicalWidth, logicalHeight, true);
    }

    void WindowImpl::show() {
        if (shown) {
            return;
        }
        shown = true;
        wl_surface_commit(surface);
        while (!configured) {
            if (wl_display_roundtrip(platform.display) < 0) {
                fail(u8"initial Wayland configure failed");
            }
        }
    }

    void WindowImpl::requestClose() {
        if (!closeRequested) {
            closeRequested = true;
            if (events != nullptr) {
                events->close();
            }
        }
    }

    bool WindowImpl::requestFrame() {
        if (frameCallback != nullptr) {
            return true;
        }
        frameCallback = wl_surface_frame(surface);
        if (frameCallback == nullptr) {
            return false;
        }
        wl_callback_add_listener(frameCallback, &frameListener, this);
        return true;
    }

    void WindowImpl::cancelFrame() {
        if (frameCallback != nullptr) {
            wl_callback_destroy(frameCallback);
            frameCallback = nullptr;
        }
    }

    void WindowImpl::frameReady(struct wl_callback* callback) {
        if (callback != frameCallback) {
            wl_callback_destroy(callback);
            return;
        }
        wl_callback_destroy(frameCallback);
        frameCallback = nullptr;
        if (events != nullptr) {
            events->frame();
        }
    }

    void WindowImpl::setTitle(StringView value) {
        title.reset();
        title.append(value.data(), value.length());
        xdg_toplevel_set_title(toplevel, title.cStr());
    }

    void WindowImpl::requestAttention() {
        platform.activate(*this);
    }

    void WindowImpl::requestRedraw() {
        if (events != nullptr) {
            events->redraw();
        }
    }

    void WindowImpl::restore() {
        xdg_toplevel_unset_maximized(toplevel);
        xdg_toplevel_unset_fullscreen(toplevel);
    }

    void WindowImpl::iconify() {
        xdg_toplevel_set_minimized(toplevel);
    }

    void WindowImpl::move(i32, i32) {
        if (platform.seat != nullptr && platform.latestSerial != 0) {
            xdg_toplevel_move(toplevel, platform.seat, platform.latestSerial);
        }
    }

    void WindowImpl::focus() {
        platform.activate(*this);
    }

    void WindowImpl::setMaximized(bool value) {
        if (value) {
            xdg_toplevel_set_maximized(toplevel);
        } else {
            xdg_toplevel_unset_maximized(toplevel);
        }
    }

    void WindowImpl::setFullscreen(bool value) {
        if (value) {
            xdg_toplevel_set_fullscreen(toplevel, nullptr);
        } else {
            xdg_toplevel_unset_fullscreen(toplevel);
        }
    }

    void WindowImpl::resize(u32 width, u32 height) {
        setLogicalSize(logicalForPixel(width), logicalForPixel(height), true);
    }

    void WindowImpl::setMinimumSize(u32 width, u32 height) {
        minimumWidth = max(1u, width);
        minimumHeight = max(1u, height);
        xdg_toplevel_set_min_size(toplevel, logicalForPixel(minimumWidth), logicalForPixel(minimumHeight));
    }

    void WindowImpl::setResizeUnit(u32 width, u32 height, u32 baseWidth, u32 baseHeight) {
        resizeUnitWidth = max(1u, width);
        resizeUnitHeight = max(1u, height);
        resizeBaseWidth = baseWidth;
        resizeBaseHeight = baseHeight;
    }

    WindowInfo WindowImpl::info() const {
        return {
            .width = pixelWidth(),
            .height = pixelHeight(),
            .screenPixelWidth = platform.outputWidth,
            .screenPixelHeight = platform.outputHeight,
            .contentScale = (float)(scaleNumerator) / scaleDenominator,
            .focused = focused,
            .maximized = maximized,
            .fullscreen = fullscreen,
            .tiled = tiled,
        };
    }

    StringView WindowImpl::receive(Offer& offer, bool primary) {
        const char* const mime = offer.mime();
        if (mime == nullptr) {
            clipboardRead.reset();
            return StringView(clipboardRead);
        }
        int pipes[2];
        if (pipe(pipes) != 0) {
            clipboardRead.reset();
            return StringView(clipboardRead);
        }
        if (primary) {
            zwp_primary_selection_offer_v1_receive(offer.primary, mime, pipes[1]);
        } else {
            wl_data_offer_receive(offer.data, mime, pipes[1]);
        }
        close(pipes[1]);
        wl_display_flush(platform.display);
        return platform.readSelection(pipes[0], clipboardRead);
    }

    StringView WindowImpl::readPrimary() {
        if (!platform.primaryContent.empty() && platform.primarySource != nullptr) {
            return StringView(platform.primaryContent);
        }
        return receive(platform.primaryOffer, true);
    }

    StringView WindowImpl::readClipboard() {
        if (!platform.clipboardContent.empty() && platform.clipboardSource != nullptr) {
            return StringView(platform.clipboardContent);
        }
        return receive(platform.clipboardOffer, false);
    }

    void WindowImpl::writePrimary(StringView content) {
        platform.setPrimary(content);
    }

    void WindowImpl::writeClipboard(StringView content) {
        platform.setClipboard(content);
    }

    void WindowImpl::pointerIcon(PointerIcon icon) {
        cursor = icon;
        updateCursor();
    }

    void WindowImpl::updateCursor() {
        platform.setCursor(*this);
    }

    void WindowImpl::pointerEntered(u32, wl_fixed_t x, wl_fixed_t y) {
        pointerMoved(x, y);
        updateCursor();
        if (input != nullptr) {
            input->pointerPresence(true);
        }
    }

    void WindowImpl::pointerLeft() {
        if (input != nullptr) {
            input->pointerPresence(false);
        }
    }

    void WindowImpl::pointerMoved(wl_fixed_t x, wl_fixed_t y) {
        pointerX = (i32)(((i64)(wl_fixed_to_double(x) * scaleNumerator)) / scaleDenominator);
        pointerY = (i32)(((i64)(wl_fixed_to_double(y) * scaleNumerator)) / scaleDenominator);
        if (input != nullptr) {
            input->pointerMotion({
                .pixelX = pointerX,
                .pixelY = pointerY,
                .modifiers = platform.modifiers(),
            });
        }
    }

    void WindowImpl::pointerButton(u32 time, u32 button, u32 state) {
        PointerButton mapped;
        switch (button) {
            case BTN_LEFT:
                mapped = PointerButton::Primary;
                break;
            case BTN_RIGHT:
                mapped = PointerButton::Secondary;
                break;
            case BTN_MIDDLE:
                mapped = PointerButton::Middle;
                break;
            case BTN_SIDE:
                mapped = PointerButton::Auxiliary1;
                break;
            case BTN_EXTRA:
                mapped = PointerButton::Auxiliary2;
                break;
            case BTN_FORWARD:
                mapped = PointerButton::Auxiliary3;
                break;
            case BTN_BACK:
                mapped = PointerButton::Auxiliary4;
                break;
            case BTN_TASK:
                mapped = PointerButton::Auxiliary5;
                break;
            default:
                return;
        }
        if (input != nullptr) {
            input->pointerButton({
                .button = mapped,
                .pressed = state == WL_POINTER_BUTTON_STATE_PRESSED,
                .pixelX = pointerX,
                .pixelY = pointerY,
                .modifiers = platform.modifiers(),
                .time = time / 1000.0,
            });
        }
    }

    void WindowImpl::pointerAxis(u32 axis, wl_fixed_t value) {
        const double converted = wl_fixed_to_double(value);
        if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
            scrollX += converted;
        } else if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
            scrollY += converted;
        }
    }

    void WindowImpl::pointerFrame() {
        if (input != nullptr && (scrollX != 0 || scrollY != 0)) {
            input->scroll({
                .x = -scrollX / 10.0,
                .y = -scrollY / 10.0,
                .pixelX = pointerX,
                .pixelY = pointerY,
                .modifiers = platform.modifiers(),
            });
        }
        scrollX = 0;
        scrollY = 0;
    }

    RenderContext WindowImpl::renderContext() const {
        return {
            .backend = RenderBackend::Wayland,
            .connection = platform.display,
            .window = surface,
        };
    }

    Platform* createNativePlatform(ObjPool& owner, PlatformEvents& events) {
        return owner.make<PlatformImpl>(owner, events);
    }
}
