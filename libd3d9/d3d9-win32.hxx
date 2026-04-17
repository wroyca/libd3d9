#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#    include <windows.h>
#    include <d3d9.h>
#    undef NOMINMAX
#  else
#    include <windows.h>
#    include <d3d9.h>
#  endif
#  undef WIN32_LEAN_AND_MEAN
#else
#  ifndef NOMINMAX
#    define NOMINMAX
#    include <windows.h>
#    include <d3d9.h>
#    undef NOMINMAX
#  else
#    include <windows.h>
#    include <d3d9.h>
#  endif
#endif
