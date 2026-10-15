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
    XC_ERR_URL,             /* not an accepted http(s) URL (docs/formats/http.md §2) */
    XC_ERR_HTTP_PROTOCOL,   /* the server's response breaks docs/formats/http.md */
    XC_ERR_NET_DNS,         /* the server's name doesn't resolve */
    XC_ERR_NET_CONNECT,     /* no connection could be opened */
    XC_ERR_NET_TIMEOUT,     /* the server stopped responding */
    XC_ERR_NET_IO,          /* the connection failed or closed early */
    XC_ERR_HTTP_NO_RANGE,   /* the server ignores Range (ADR 0004) */
    XC_ERR_HTTP_DENIED,     /* 401/403 (ADR 0006: reported, never worked around) */
    XC_ERR_HTTP_NOT_FOUND,  /* 404/410 */
    XC_ERR_HTTP_STATUS,     /* any other error status; see xc_http_client.last_status */
    XC_ERR_HTTP_REDIRECTS,  /* more than 5 redirects */
    XC_ERR_HTTP_CHANGED,    /* the remote file changed between requests */
    XC_ERR_UNSUPPORTED,     /* not supported by this version (e.g. https before M5) */
    XC_ERR_CREDENTIALS,     /* the credentials file is invalid (credentials.md §7) */
    XC_ERR_MAP,             /* the map file itself is rejected (map.md §7) */
    XC_ERR_COUNT_           /* number of codes; not an error */
} xc_err;

/* Returns a short, user-readable description. Never NULL, even for unknown values. */
const char *xc_err_str(xc_err err);

#ifdef __cplusplus
}
#endif

#endif /* XCAT_ERROR_H */
