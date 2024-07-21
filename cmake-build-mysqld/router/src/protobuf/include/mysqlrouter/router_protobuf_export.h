
#ifndef ROUTER_PROTOBUF_EXPORT_H
#define ROUTER_PROTOBUF_EXPORT_H

#ifdef ROUTER_PROTOBUF_STATIC_DEFINE
#  define ROUTER_PROTOBUF_EXPORT
#  define ROUTER_PROTOBUF_NO_EXPORT
#else
#  ifndef ROUTER_PROTOBUF_EXPORT
#    ifdef router_protobuf_EXPORTS
        /* We are building this library */
#      define ROUTER_PROTOBUF_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define ROUTER_PROTOBUF_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef ROUTER_PROTOBUF_NO_EXPORT
#    define ROUTER_PROTOBUF_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef ROUTER_PROTOBUF_DEPRECATED
#  define ROUTER_PROTOBUF_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef ROUTER_PROTOBUF_DEPRECATED_EXPORT
#  define ROUTER_PROTOBUF_DEPRECATED_EXPORT ROUTER_PROTOBUF_EXPORT ROUTER_PROTOBUF_DEPRECATED
#endif

#ifndef ROUTER_PROTOBUF_DEPRECATED_NO_EXPORT
#  define ROUTER_PROTOBUF_DEPRECATED_NO_EXPORT ROUTER_PROTOBUF_NO_EXPORT ROUTER_PROTOBUF_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef ROUTER_PROTOBUF_NO_DEPRECATED
#    define ROUTER_PROTOBUF_NO_DEPRECATED
#  endif
#endif

#endif /* ROUTER_PROTOBUF_EXPORT_H */
