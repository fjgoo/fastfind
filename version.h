/* fastfind version — single source of truth.
   Edit the four numbers below; the display string ("1.0.0.0"), the About
   dialog and the exe version resource are all derived from them. */
#pragma once

#define FF_VER_MAJOR 1
#define FF_VER_MINOR 0
#define FF_VER_PATCH 0
#define FF_VER_BUILD 0

/* "1.0.0.0" built from the numbers so nothing can drift apart */
#define FF_STR_(x) #x
#define FF_STR(x)  FF_STR_(x)
#define FF_VER_STRING FF_STR(FF_VER_MAJOR) "." FF_STR(FF_VER_MINOR) "." \
                      FF_STR(FF_VER_PATCH) "." FF_STR(FF_VER_BUILD)

/* wide form for C source */
#define FF_WIDE_(x) L##x
#define FF_WIDE(x)  FF_WIDE_(x)
#define FF_VER_WSTRING FF_WIDE(FF_VER_STRING)

#define FF_PRODUCT_NAME "fastfind"
#define FF_PRODUCT_WNAME FF_WIDE(FF_PRODUCT_NAME)
#define FF_DESCRIPTION  "fastfind - instant filename search"
