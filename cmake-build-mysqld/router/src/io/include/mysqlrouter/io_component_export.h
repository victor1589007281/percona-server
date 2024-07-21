
#ifndef IO_COMPONENT_EXPORT_H
#define IO_COMPONENT_EXPORT_H

#ifdef IO_COMPONENT_STATIC_DEFINE
#  define IO_COMPONENT_EXPORT
#  define IO_COMPONENT_NO_EXPORT
#else
#  ifndef IO_COMPONENT_EXPORT
#    ifdef io_component_EXPORTS
        /* We are building this library */
#      define IO_COMPONENT_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define IO_COMPONENT_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef IO_COMPONENT_NO_EXPORT
#    define IO_COMPONENT_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef IO_COMPONENT_DEPRECATED
#  define IO_COMPONENT_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef IO_COMPONENT_DEPRECATED_EXPORT
#  define IO_COMPONENT_DEPRECATED_EXPORT IO_COMPONENT_EXPORT IO_COMPONENT_DEPRECATED
#endif

#ifndef IO_COMPONENT_DEPRECATED_NO_EXPORT
#  define IO_COMPONENT_DEPRECATED_NO_EXPORT IO_COMPONENT_NO_EXPORT IO_COMPONENT_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef IO_COMPONENT_NO_DEPRECATED
#    define IO_COMPONENT_NO_DEPRECATED
#  endif
#endif

#endif /* IO_COMPONENT_EXPORT_H */
