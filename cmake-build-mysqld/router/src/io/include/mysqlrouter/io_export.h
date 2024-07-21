
#ifndef IO_EXPORT_H
#define IO_EXPORT_H

#ifdef IO_STATIC_DEFINE
#  define IO_EXPORT
#  define IO_NO_EXPORT
#else
#  ifndef IO_EXPORT
#    ifdef io_EXPORTS
        /* We are building this library */
#      define IO_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define IO_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef IO_NO_EXPORT
#    define IO_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef IO_DEPRECATED
#  define IO_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef IO_DEPRECATED_EXPORT
#  define IO_DEPRECATED_EXPORT IO_EXPORT IO_DEPRECATED
#endif

#ifndef IO_DEPRECATED_NO_EXPORT
#  define IO_DEPRECATED_NO_EXPORT IO_NO_EXPORT IO_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef IO_NO_DEPRECATED
#    define IO_NO_DEPRECATED
#  endif
#endif

#endif /* IO_EXPORT_H */
