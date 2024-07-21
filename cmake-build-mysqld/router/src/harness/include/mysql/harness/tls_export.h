
#ifndef HARNESS_TLS_EXPORT_H
#define HARNESS_TLS_EXPORT_H

#ifdef HARNESS_TLS_STATIC_DEFINE
#  define HARNESS_TLS_EXPORT
#  define HARNESS_TLS_NO_EXPORT
#else
#  ifndef HARNESS_TLS_EXPORT
#    ifdef harness_tls_EXPORTS
        /* We are building this library */
#      define HARNESS_TLS_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define HARNESS_TLS_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef HARNESS_TLS_NO_EXPORT
#    define HARNESS_TLS_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef HARNESS_TLS_DEPRECATED
#  define HARNESS_TLS_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef HARNESS_TLS_DEPRECATED_EXPORT
#  define HARNESS_TLS_DEPRECATED_EXPORT HARNESS_TLS_EXPORT HARNESS_TLS_DEPRECATED
#endif

#ifndef HARNESS_TLS_DEPRECATED_NO_EXPORT
#  define HARNESS_TLS_DEPRECATED_NO_EXPORT HARNESS_TLS_NO_EXPORT HARNESS_TLS_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef HARNESS_TLS_NO_DEPRECATED
#    define HARNESS_TLS_NO_DEPRECATED
#  endif
#endif

#endif /* HARNESS_TLS_EXPORT_H */
