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

#pragma once

#include <gdk/gdk.h>

namespace xoj::util::quartz {

#if defined(__APPLE__) && defined(GDK_WINDOWING_QUARTZ)

/**
 * @brief Install a global GDK event filter that records the exact (sub-pixel) pointer position of every
 * native macOS event. Call once after GTK initialization.
 *
 * The Quartz backend of GDK truncates the coordinates of the GdkEvents it builds to whole pixels
 * (https://github.com/xournalpp/xournalpp/issues/5451). The filter sees the native NSEvent before that
 * truncation happens and stores its exact position, keyed by the event timestamp (which GDK copies
 * verbatim into the GdkEvent).
 */
void installEventFilter();

/**
 * @brief Get the fractional (sub-pixel) part of the pointer position of a GDK event.
 * @param event The GDK event whose native counterpart was recorded by the event filter
 * @param fracX[out] Fractional part of the x coordinate (to be added to the integer coordinate)
 * @param fracY[out] Fractional part of the y coordinate (to be added to the integer coordinate)
 * @return true if the native position was recorded for this event, false otherwise
 */
bool getSubpixelOffset(GdkEvent* event, double& fracX, double& fracY);

#else

inline void installEventFilter() {}

inline bool getSubpixelOffset(GdkEvent*, double&, double&) { return false; }

#endif

}  // namespace xoj::util::quartz
