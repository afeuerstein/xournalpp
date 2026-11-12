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
#include <cstring>
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


bool getClipboardDataForTarget(const char* targetName, std::string& out) {
    if (targetName == nullptr) {
        return false;
    }
    static const SEL generalPasteboardSel = sel_registerName("generalPasteboard");
    static const SEL typesSel = sel_registerName("types");
    static const SEL countSel = sel_registerName("count");
    static const SEL objectAtIndexSel = sel_registerName("objectAtIndex:");
    static const SEL utf8StringSel = sel_registerName("UTF8String");
    static const SEL dataForTypeSel = sel_registerName("dataForType:");
    static const SEL lengthSel = sel_registerName("length");
    static const SEL bytesSel = sel_registerName("bytes");
    static const SEL typeWithIdentifierSel = sel_registerName("typeWithIdentifier:");
    static const SEL preferredMIMETypeSel = sel_registerName("preferredMIMEType");

    const Class pbClass = objc_getClass("NSPasteboard");
    if (pbClass == nil) {
        return false;
    }
    const id pb = objcSendId(reinterpret_cast<id>(pbClass), generalPasteboardSel);
    if (pb == nil) {
        return false;
    }
    const id types = objcSendId(pb, typesSel);
    if (types == nil) {
        return false;
    }
    const Class utTypeClass = objc_getClass("UTType");  // macOS 11+
    const unsigned long n = reinterpret_cast<unsigned long (*)(id, SEL)>(objc_msgSend)(types, countSel);
    for (unsigned long i = 0; i < n; i++) {
        const id type = reinterpret_cast<id (*)(id, SEL, unsigned long)>(objc_msgSend)(types, objectAtIndexSel, i);
        if (type == nil) {
            continue;
        }
        const char* typeCStr = reinterpret_cast<const char* (*)(id, SEL)>(objc_msgSend)(type, utf8StringSel);
        bool match = typeCStr != nullptr && std::strcmp(targetName, typeCStr) == 0;
        if (!match && utTypeClass != nil) {
            // A custom GDK target is stored under a dynamic UTI whose preferred MIME type is the
            // target name (this is how GDK maps pasteboard types back to atoms for the TARGETS list)
            const id ut = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(reinterpret_cast<id>(utTypeClass), typeWithIdentifierSel, type);
            if (ut != nil) {
                const id mime = objcSendId(ut, preferredMIMETypeSel);
                if (mime != nil) {
                    const char* mimeCStr = reinterpret_cast<const char* (*)(id, SEL)>(objc_msgSend)(mime, utf8StringSel);
                    match = mimeCStr != nullptr && std::strcmp(targetName, mimeCStr) == 0;
                }
            }
        }
        if (!match) {
            continue;
        }
        const id data = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(pb, dataForTypeSel, type);
        if (data == nil) {
            return false;
        }
        const unsigned long len = reinterpret_cast<unsigned long (*)(id, SEL)>(objc_msgSend)(data, lengthSel);
        const void* bytes = reinterpret_cast<const void* (*)(id, SEL)>(objc_msgSend)(data, bytesSel);
        if (bytes == nullptr || len == 0) {
            return false;
        }
        out.assign(static_cast<const char*>(bytes), len);
        return true;
    }
    return false;
}

}  // namespace xoj::util::quartz

#endif  // defined(__APPLE__) && defined(GDK_WINDOWING_QUARTZ)
