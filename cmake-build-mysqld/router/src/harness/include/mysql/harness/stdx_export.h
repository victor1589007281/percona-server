
#ifndef HARNESS_STDX_EXPORT_H
#define HARNESS_STDX_EXPORT_H

#ifdef HARNESS_STDX_STATIC_DEFINE
#  define HARNESS_STDX_EXPORT
#  define HARNESS_STDX_NO_EXPORT
#else
#  ifndef HARNESS_STDX_EXPORT
#    ifdef harness_stdx_EXPORTS
        /* We are building this library */
#      define HARNESS_STDX_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define HARNESS_STDX_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef HARNESS_STDX_NO_EXPORT
#    define HARNESS_STDX_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef HARNESS_STDX_DEPRECATED
#  define HARNESS_STDX_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef HARNESS_STDX_DEPRECATED_EXPORT
#  define HARNESS_STDX_DEPRECATED_EXPORT HARNESS_STDX_EXPORT HARNESS_STDX_DEPRECATED
#endif

#ifndef HARNESS_STDX_DEPRECATED_NO_EXPORT
#  define HARNESS_STDX_DEPRECATED_NO_EXPORT HARNESS_STDX_NO_EXPORT HARNESS_STDX_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef HARNESS_STDX_NO_DEPRECATED
#    define HARNESS_STDX_NO_DEPRECATED
#  endif
#endif

#endif /* HARNESS_STDX_EXPORT_H */
