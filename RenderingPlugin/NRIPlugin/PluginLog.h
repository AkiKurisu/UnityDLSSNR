#pragma once

// Instance and device lifecycle messages are opt-in through NRIPLUGIN_VERBOSE_LOG; problems always log.
#ifdef NRIPLUGIN_VERBOSE_LOG
#define LOG_VERBOSE(msg) LOG(msg)
#else
#define LOG_VERBOSE(msg) ((void)0)
#endif
