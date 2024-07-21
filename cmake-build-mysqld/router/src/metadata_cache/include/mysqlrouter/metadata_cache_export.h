
#ifndef METADATA_CACHE_EXPORT_H
#define METADATA_CACHE_EXPORT_H

#ifdef METADATA_CACHE_STATIC_DEFINE
#  define METADATA_CACHE_EXPORT
#  define METADATA_CACHE_NO_EXPORT
#else
#  ifndef METADATA_CACHE_EXPORT
#    ifdef metadata_cache_EXPORTS
        /* We are building this library */
#      define METADATA_CACHE_EXPORT __attribute__((visibility("default")))
#    else
        /* We are using this library */
#      define METADATA_CACHE_EXPORT __attribute__((visibility("default")))
#    endif
#  endif

#  ifndef METADATA_CACHE_NO_EXPORT
#    define METADATA_CACHE_NO_EXPORT __attribute__((visibility("hidden")))
#  endif
#endif

#ifndef METADATA_CACHE_DEPRECATED
#  define METADATA_CACHE_DEPRECATED __attribute__ ((__deprecated__))
#endif

#ifndef METADATA_CACHE_DEPRECATED_EXPORT
#  define METADATA_CACHE_DEPRECATED_EXPORT METADATA_CACHE_EXPORT METADATA_CACHE_DEPRECATED
#endif

#ifndef METADATA_CACHE_DEPRECATED_NO_EXPORT
#  define METADATA_CACHE_DEPRECATED_NO_EXPORT METADATA_CACHE_NO_EXPORT METADATA_CACHE_DEPRECATED
#endif

#if 0 /* DEFINE_NO_DEPRECATED */
#  ifndef METADATA_CACHE_NO_DEPRECATED
#    define METADATA_CACHE_NO_DEPRECATED
#  endif
#endif

#endif /* METADATA_CACHE_EXPORT_H */
