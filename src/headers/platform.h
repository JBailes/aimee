/*
 * platform.h: platform detection and common portability macros.
 *
 * Defines AIMEE_LINUX, AIMEE_MACOS, AIMEE_WINDOWS so downstream code
 * can select the right implementation at compile time.
 */
#ifndef DEC_PLATFORM_H
#define DEC_PLATFORM_H 1

/* --- Platform detection --- */

#if defined(_WIN32) || defined(_WIN64)
#define AIMEE_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
#define AIMEE_MACOS 1
#elif defined(__linux__)
#define AIMEE_LINUX 1
#else
#error "Unsupported platform: expected Linux, macOS, or Windows"
#endif

/* AIMEE_POSIX is set on any Unix-like system (Linux, macOS) */
#if defined(AIMEE_LINUX) || defined(AIMEE_MACOS)
#define AIMEE_POSIX 1
#endif

/* --- Portable handle type --- */

#ifdef AIMEE_WINDOWS
#include <windows.h>
typedef HANDLE platform_fd_t;
#define PLATFORM_INVALID_FD INVALID_HANDLE_VALUE
#else
typedef int platform_fd_t;
#define PLATFORM_INVALID_FD (-1)
#endif

#endif /* DEC_PLATFORM_H */
