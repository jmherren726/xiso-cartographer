#ifndef XCAT_ERROR_H
#define XCAT_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

/* Result of every fallible libxcat function. Results come back through out-params. */
typedef enum xc_err {
    XC_OK = 0,
    XC_ERR_INVALID_ARG,     /* caller bug: NULL pointer or impossible argument */
    XC_ERR_NOMEM,           /* allocation failed */
    XC_ERR_IO,              /* the underlying read or write failed */
    XC_ERR_RANGE,           /* a read reached outside the source */
    XC_ERR_NOT_XDVDFS,      /* no Xbox volume descriptor at any known layout */
    XC_ERR_CORRUPT,         /* the data violates its format (or is hostile) */
    XC_ERR_LIMIT,           /* the data exceeds a configured safety limit */
    XC_ERR_CANCELLED,       /* the caller asked to stop (e.g. a progress callback) */
    XC_ERR_NOT_INSTALLABLE, /* the game's files can't be stored on FATX (ADR 0008) */
    XC_ERR_COUNT_           /* number of codes; not an error */
} xc_err;

/* Returns a short, user-readable description. Never NULL, even for unknown values. */
const char *xc_err_str(xc_err err);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ERROR_H */
