#ifndef DUSK_NET_COOP_LOG_H
#define DUSK_NET_COOP_LOG_H

// Debug logging for the co-op multiplayer subsystem. Every line goes through
// COOP_LOG / COOP_TRACE so it carries a consistent "[coop]" tag — pipe a log
// through scripts/coop_log_filter.py (or grep "[coop]") to extract just the
// co-op activity from the resource-load / fpc churn that fills the log.
//
//   COOP_LOG(fmt, ...)   - normal co-op events (connect, puppet spawn/despawn,
//                          save sync, scene transitions, anim/pose changes).
//                          Compiled out when DUSK_COOP_LOG is 0.
//   COOP_TRACE(fmt, ...)  - per-tick / high-frequency detail (puppet execute
//                          heartbeat, why-not-spawning, etc.). Compiled out
//                          unless DUSK_COOP_LOG_VERBOSE is 1 — flip it on to
//                          chase a specific issue, then flip it back.
//
// `fmt` must be a string literal (the macros prepend "[coop] " by literal
// concatenation). Both expand to DuskLog.debug, so they only surface at
// debug-or-lower log levels.

#include "dusk/logging.h"

#ifndef DUSK_COOP_LOG
#  define DUSK_COOP_LOG 1
#endif
#ifndef DUSK_COOP_LOG_VERBOSE
#  define DUSK_COOP_LOG_VERBOSE 0
#endif

// `, ##__VA_ARGS__` swallows the leading comma when no args are passed — works
// on GCC/Clang and MSVC's traditional preprocessor (the project doesn't enable
// /Zc:preprocessor, so __VA_OPT__ isn't available).
#if DUSK_COOP_LOG
#  define COOP_LOG(fmt, ...) DuskLog.debug("[coop] " fmt, ##__VA_ARGS__)
#else
#  define COOP_LOG(fmt, ...) ((void)0)
#endif

#if DUSK_COOP_LOG && DUSK_COOP_LOG_VERBOSE
#  define COOP_TRACE(fmt, ...) DuskLog.debug("[coop] " fmt, ##__VA_ARGS__)
#else
#  define COOP_TRACE(fmt, ...) ((void)0)
#endif

#endif  // DUSK_NET_COOP_LOG_H
