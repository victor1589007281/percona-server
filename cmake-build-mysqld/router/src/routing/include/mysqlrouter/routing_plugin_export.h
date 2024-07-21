
#ifndef ROUTING_PLUGIN_EXPORT_H
#define ROUTING_PLUGIN_EXPORT_H

#ifdef ROUTING_PLUGIN_STATIC_DEFINE
#  define ROUTING_PLUGIN_EXPORT
#  define ROUTING_PLUGIN_NO_EXPORT
#else
#  ifndef ROUTING_PLUGIN_EXPORT
#    ifdef routing_plugin_EXPORTS
        /* We are building this library */
#      define ROUTING_PLUGIN_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define ROUTING_PLUGIN_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef ROUTING_PLUGIN_NO_EXPORT
#    define ROUTING_PLUGIN_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef ROUTING_PLUGIN_DEPRECATED
#  define ROUTING_PLUGIN_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef ROUTING_PLUGIN_DEPRECATED_EXPORT
#  define ROUTING_PLUGIN_DEPRECATED_EXPORT ROUTING_PLUGIN_EXPORT ROUTING_PLUGIN_DEPRECATED
#endif

#ifndef ROUTING_PLUGIN_DEPRECATED_NO_EXPORT
#  define ROUTING_PLUGIN_DEPRECATED_NO_EXPORT ROUTING_PLUGIN_NO_EXPORT ROUTING_PLUGIN_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef ROUTING_PLUGIN_NO_DEPRECATED
#    define ROUTING_PLUGIN_NO_DEPRECATED
#  endif
#endif

#endif /* ROUTING_PLUGIN_EXPORT_H */
