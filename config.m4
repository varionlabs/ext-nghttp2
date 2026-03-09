PHP_ARG_ENABLE([nghttp2],
  [whether to enable nghttp2 extension],
  [AS_HELP_STRING([--enable-nghttp2], [Enable nghttp2 extension])],
  [no])

if test "$PHP_NGHTTP2" != "no"; then
  PKG_CHECK_MODULES([LIBNGHTTP2], [libnghttp2 >= 1.52.0], [
    PHP_EVAL_INCLINE([$LIBNGHTTP2_CFLAGS])
    PHP_EVAL_LIBLINE([$LIBNGHTTP2_LIBS], [NGHTTP2_SHARED_LIBADD])
  ], [
    AC_MSG_ERROR([libnghttp2 >= 1.52.0 is required])
  ])

  PHP_SUBST([NGHTTP2_SHARED_LIBADD])
  PHP_NEW_EXTENSION([nghttp2], [nghttp2.c session.c event.c], [$ext_shared])
fi
