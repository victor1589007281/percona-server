
#ifndef ROUTER_OPENSSL_EXPORT_H
#define ROUTER_OPENSSL_EXPORT_H

#ifdef ROUTER_OPENSSL_STATIC_DEFINE
#  define ROUTER_OPENSSL_EXPORT
#  define ROUTER_OPENSSL_NO_EXPORT
#else
#  ifndef ROUTER_OPENSSL_EXPORT
#    ifdef router_openssl_EXPORTS
        /* We are building this library */
#      define ROUTER_OPENSSL_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define ROUTER_OPENSSL_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef ROUTER_OPENSSL_NO_EXPORT
#    define ROUTER_OPENSSL_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef ROUTER_OPENSSL_DEPRECATED
#  define ROUTER_OPENSSL_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef ROUTER_OPENSSL_DEPRECATED_EXPORT
#  define ROUTER_OPENSSL_DEPRECATED_EXPORT ROUTER_OPENSSL_EXPORT ROUTER_OPENSSL_DEPRECATED
#endif

#ifndef ROUTER_OPENSSL_DEPRECATED_NO_EXPORT
#  define ROUTER_OPENSSL_DEPRECATED_NO_EXPORT ROUTER_OPENSSL_NO_EXPORT ROUTER_OPENSSL_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef ROUTER_OPENSSL_NO_DEPRECATED
#    define ROUTER_OPENSSL_NO_DEPRECATED
#  endif
#endif

#endif /* ROUTER_OPENSSL_EXPORT_H */
