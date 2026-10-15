#include "xcat/error.h"

const char *xc_err_str(xc_err err)
{
    switch (err) {
    case XC_OK:
        return "OK";
    case XC_ERR_INVALID_ARG:
        return "Invalid argument";
    case XC_ERR_NOMEM:
        return "Out of memory";
    case XC_ERR_IO:
        return "Read or write failed";
    case XC_ERR_RANGE:
        return "Tried to read past the end of the image";
    case XC_ERR_NOT_XDVDFS:
        return "Not an Xbox disc image";
    case XC_ERR_CORRUPT:
        return "The image is damaged or invalid";
    case XC_ERR_LIMIT:
        return "The image exceeds a safety limit";
    case XC_ERR_CANCELLED:
        return "Cancelled";
    case XC_ERR_NOT_INSTALLABLE:
        return "This game has files the Xbox hard disk can't store";
    case XC_ERR_URL:
        return "Invalid or unsupported URL";
    case XC_ERR_HTTP_PROTOCOL:
        return "The server sent an invalid response";
    case XC_ERR_COUNT_:
        break;
    }
    return "Unknown error";
}
