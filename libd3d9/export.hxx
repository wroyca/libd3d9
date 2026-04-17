#pragma once

#if defined(LIBD3D9_STATIC)
#  define LIBD3D9_SYMEXPORT
#elif defined(LIBD3D9_STATIC_BUILD)
#  define LIBD3D9_SYMEXPORT
#elif defined(LIBD3D9_SHARED)
#  ifdef _WIN32
#    define LIBD3D9_SYMEXPORT __declspec (dllimport)
#  else
#    define LIBD3D9_SYMEXPORT
#  endif
#elif defined(LIBD3D9_SHARED_BUILD)
#  ifdef _WIN32
#    define LIBD3D9_SYMEXPORT __declspec (dllexport)
#  else
#    define LIBD3D9_SYMEXPORT
#  endif
#else
// If none of the above macros are defined, then we assume we are being used
// by some third-party build system that cannot/doesn't signal the library
// type being linked.
//
#  error define LIBD3D9_STATIC or LIBD3D9_SHARED preprocessor macro to signal libd3d9 library type being linked
#endif
