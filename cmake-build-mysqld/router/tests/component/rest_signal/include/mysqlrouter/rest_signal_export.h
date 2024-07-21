
#ifndef REST_SIGNAL_EXPORT_H
#define REST_SIGNAL_EXPORT_H

#ifdef REST_SIGNAL_STATIC_DEFINE
#  define REST_SIGNAL_EXPORT
#  define REST_SIGNAL_NO_EXPORT
#else
#  ifndef REST_SIGNAL_EXPORT
#    ifdef rest_signal_EXPORTS
        /* We are building this library */
#      define REST_SIGNAL_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define REST_SIGNAL_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef REST_SIGNAL_NO_EXPORT
#    define REST_SIGNAL_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef REST_SIGNAL_DEPRECATED
#  define REST_SIGNAL_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef REST_SIGNAL_DEPRECATED_EXPORT
#  define REST_SIGNAL_DEPRECATED_EXPORT REST_SIGNAL_EXPORT REST_SIGNAL_DEPRECATED
#endif

#ifndef REST_SIGNAL_DEPRECATED_NO_EXPORT
#  define REST_SIGNAL_DEPRECATED_NO_EXPORT REST_SIGNAL_NO_EXPORT REST_SIGNAL_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef REST_SIGNAL_NO_DEPRECATED
#    define REST_SIGNAL_NO_DEPRECATED
#  endif
#endif

#endif /* REST_SIGNAL_EXPORT_H */
