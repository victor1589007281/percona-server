
#ifndef DESTINATION_STATUS_EXPORT_H
#define DESTINATION_STATUS_EXPORT_H

#ifdef DESTINATION_STATUS_STATIC_DEFINE
#  define DESTINATION_STATUS_EXPORT
#  define DESTINATION_STATUS_NO_EXPORT
#else
#  ifndef DESTINATION_STATUS_EXPORT
#    ifdef destination_status_EXPORTS
        /* We are building this library */
#      define DESTINATION_STATUS_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define DESTINATION_STATUS_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef DESTINATION_STATUS_NO_EXPORT
#    define DESTINATION_STATUS_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef DESTINATION_STATUS_DEPRECATED
#  define DESTINATION_STATUS_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef DESTINATION_STATUS_DEPRECATED_EXPORT
#  define DESTINATION_STATUS_DEPRECATED_EXPORT DESTINATION_STATUS_EXPORT DESTINATION_STATUS_DEPRECATED
#endif

#ifndef DESTINATION_STATUS_DEPRECATED_NO_EXPORT
#  define DESTINATION_STATUS_DEPRECATED_NO_EXPORT DESTINATION_STATUS_NO_EXPORT DESTINATION_STATUS_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef DESTINATION_STATUS_NO_DEPRECATED
#    define DESTINATION_STATUS_NO_DEPRECATED
#  endif
#endif

#endif /* DESTINATION_STATUS_EXPORT_H */
