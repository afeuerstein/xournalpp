/*
 * Xournal++
 *
 * Recovery of sub-pixel pointer positions on macOS (Quartz backend of GDK)
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#include "util/QuartzInputPrecision.h"

#if defined(__APPLE__) && defined(GDK_WINDOWING_QUARTZ)

#include <cmath>
#include <cstdint>
#include <mutex>
#include <unordered_map>

#include <glib.h>

#include <objc/message.h>
#include <objc/objc.h>
#include <objc/runtime.h>

namespace {

/**
 * Layout of NSPoint (CGFloat is double on 64-bit platforms).
 * AppKit is not included here on purpose: the few NSEvent accessors needed are called through the
 * Objective-C runtime, which works from plain C++.
 */
struct XojNSPoint {
    double x;
    double y;
};

id objcSendId(id obj, SEL sel) {
    return reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(obj, sel);
}

double objcSendDouble(id obj, SEL sel) {
    return reinterpret_cast<double (*)(id, SEL)>(objc_msgSend)(obj, sel);
}

XojNSPoint objcSendPoint(id obj, SEL sel) {
    return reinterpret_cast<XojNSPoint (*)(id, SEL)>(objc_msgSend)(obj, sel);
}

struct NativeEvent {
    double x;  ///< [NSEvent locationInWindow].x
    double y;  ///< [NSEvent locationInWindow].y
    gint64 recordTimeUs;
};

std::mutex entriesMutex;
/**
 * Exact pointer positions of the last native events, keyed by the timestamp GDK will put in the GdkEvent
 * (same conversion as GDK's get_time_from_ns_event()).
 */
std::unordered_map<guint32, NativeEvent> entries;

constexpr gint64 MAX_ENTRY_AGE_US = G_USEC_PER_SEC;  // 1 s
constexpr size_t MAX_ENTRIES = 2048;

/**
 * Global GDK event filter. On the Quartz backend, GdkXEvent is the native NSEvent*, and filters run
 * before GDK truncates the event coordinates to integers (gdk/quartz/gdkevents-quartz.c).
 */
GdkFilterReturn quartzNativeEventFilter(GdkXEvent* xevent, GdkEvent*, gpointer) {
    const id nsevent = reinterpret_cast<id>(xevent);
    if (nsevent == nil) {
        return GDK_FILTER_CONTINUE;
    }
    static const SEL windowSel = sel_registerName("window");
    static const SEL timestampSel = sel_registerName("timestamp");
    static const SEL locationInWindowSel = sel_registerName("locationInWindow");
    if (objcSendId(nsevent, windowSel) == nil) {
        // Events without a window have no meaningful locationInWindow
        return GDK_FILTER_CONTINUE;
    }
    const XojNSPoint p = objcSendPoint(nsevent, locationInWindowSel);
    const guint32 ts = static_cast<guint32>(static_cast<guint64>(objcSendDouble(nsevent, timestampSel) * 1000.0));
    const gint64 nowUs = g_get_monotonic_time();
    std::lock_guard<std::mutex> lock(entriesMutex);
    if (entries.size() >= MAX_ENTRIES) {
        for (auto it = entries.begin(); it != entries.end();) {
            if (nowUs - it->second.recordTimeUs > MAX_ENTRY_AGE_US) {
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
    }
    entries[ts] = NativeEvent{p.x, p.y, nowUs};
    return GDK_FILTER_CONTINUE;
}

}  // namespace

namespace xoj::util::quartz {

void installEventFilter() {
    static std::once_flag once;
    std::call_once(once, [] { gdk_window_add_filter(nullptr, &quartzNativeEventFilter, nullptr); });
}

bool getSubpixelOffset(GdkEvent* event, double& fracX, double& fracY) {
    if (event == nullptr) {
        return false;
    }
    const guint32 ts = gdk_event_get_time(event);
    if (ts == GDK_CURRENT_TIME) {
        return false;
    }
    double px = 0.0;
    double py = 0.0;
    {
        std::lock_guard<std::mutex> lock(entriesMutex);
        const auto it = entries.find(ts);
        if (it == entries.end() || g_get_monotonic_time() - it->second.recordTimeUs > MAX_ENTRY_AGE_US) {
            return false;
        }
        px = it->second.x;
        py = it->second.y;
    }
    GdkWindow* window = gdk_event_get_window(event);
    if (window == nullptr) {
        return false;
    }
    // Reproduce the coordinate conversion of GDK's Quartz backend (get_window_point_from_screen_point):
    // x = point.x, y = toplevel->height - point.y. The integer part is already in the GdkEvent; only the
    // fractional part is missing there. Window offsets within a toplevel are whole pixels, so the
    // fractional part is the same for the event's window and its toplevel.
    const double height = static_cast<double>(gdk_window_get_height(gdk_window_get_toplevel(window)));
    const double x = px;
    const double y = height - py;
    fracX = x - std::trunc(x);
    fracY = y - std::trunc(y);
    return true;
}

}  // namespace xoj::util::quartz

#endif  // defined(__APPLE__) && defined(GDK_WINDOWING_QUARTZ)
