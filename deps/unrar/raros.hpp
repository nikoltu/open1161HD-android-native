#ifndef _RAR_RAROS_
#define _RAR_RAROS_

#if defined(__WIN32__) || defined(_WIN32)
  #define _WIN_ALL // Defined for all Windows platforms, 32 and 64 bit, mobile and desktop.

  #define _WIN_X64STR   L"x64"
  #define _WIN_A64STR   L"ARM64"
  #define _WIN_X86STR   L"x86"

  #ifdef _M_X64
    #define _WIN_64
    #define _WIN_X64
    #define _WIN_ARSTR _WIN_X64STR
  #elif defined(_M_ARM64)
    #define _WIN_64
    #define _WIN_ARM64
    #define _WIN_ARSTR _WIN_A64STR
  #else
    #define _WIN_32
    #define _WIN_X86
    #define _WIN_ARSTR _WIN_X86STR
  #endif
#endif

#if defined(ANDROID) || defined(__ANDROID__)
  #define _UNIX
  #define _ANDROID
#endif

#ifdef __APPLE__
  #define _UNIX
  #define _APPLE
#endif

#if !defined(_WIN_ALL) && !defined(_UNIX)
  #define _UNIX
#endif

#endif
