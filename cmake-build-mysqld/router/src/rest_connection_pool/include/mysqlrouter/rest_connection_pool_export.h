
#ifndef REST_CONNECTION_POOL_EXPORT_H
#define REST_CONNECTION_POOL_EXPORT_H

#ifdef REST_CONNECTION_POOL_STATIC_DEFINE
#  define REST_CONNECTION_POOL_EXPORT
#  define REST_CONNECTION_POOL_NO_EXPORT
#else
#  ifndef REST_CONNECTION_POOL_EXPORT
#    ifdef rest_connection_pool_EXPORTS
        /* We are building this library */
#      define REST_CONNECTION_POOL_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define REST_CONNECTION_POOL_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef REST_CONNECTION_POOL_NO_EXPORT
#    define REST_CONNECTION_POOL_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef REST_CONNECTION_POOL_DEPRECATED
#  define REST_CONNECTION_POOL_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef REST_CONNECTION_POOL_DEPRECATED_EXPORT
#  define REST_CONNECTION_POOL_DEPRECATED_EXPORT REST_CONNECTION_POOL_EXPORT REST_CONNECTION_POOL_DEPRECATED
#endif

#ifndef REST_CONNECTION_POOL_DEPRECATED_NO_EXPORT
#  define REST_CONNECTION_POOL_DEPRECATED_NO_EXPORT REST_CONNECTION_POOL_NO_EXPORT REST_CONNECTION_POOL_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef REST_CONNECTION_POOL_NO_DEPRECATED
#    define REST_CONNECTION_POOL_NO_DEPRECATED
#  endif
#endif

#endif /* REST_CONNECTION_POOL_EXPORT_H */
