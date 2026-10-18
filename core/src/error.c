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
    case XC_ERR_NET_DNS:
        return "Couldn't find the server (name lookup failed)";
    case XC_ERR_NET_CONNECT:
        return "Couldn't connect to the server";
    case XC_ERR_NET_TIMEOUT:
        return "The server stopped responding";
    case XC_ERR_NET_IO:
        return "The network connection failed";
    case XC_ERR_HTTP_NO_RANGE:
        return "This server doesn't support resumable downloads (HTTP Range)";
    case XC_ERR_HTTP_DENIED:
        return "Access denied by the server (login missing or expired?)";
    case XC_ERR_HTTP_NOT_FOUND:
        return "The file wasn't found on the server";
    case XC_ERR_HTTP_STATUS:
        return "The server returned an error";
    case XC_ERR_HTTP_REDIRECTS:
        return "Too many redirects";
    case XC_ERR_HTTP_CHANGED:
        return "The file on the server changed during the download";
    case XC_ERR_UNSUPPORTED:
        return "Not supported by this version";
    case XC_ERR_CREDENTIALS:
        return "The credentials file is invalid, so no credentials are used";
    case XC_ERR_MAP:
        return "The map file can't be used";
    case XC_ERR_NO_SPACE:
        return "Not enough free space on the drive";
    case XC_ERR_VERIFY:
        return "An installed file doesn't match what was downloaded";
    case XC_ERR_FOLDER:
        return "No free install folder name left for this title";
    case XC_ERR_TLS:
        return "The secure (HTTPS) connection failed";
    case XC_ERR_TLS_CERT:
        return "The server's certificate isn't trusted (unknown issuer or wrong name)";
    case XC_ERR_TLS_CERT_TIME:
        return "The server's certificate isn't valid at the current time (is the clock right?)";
    case XC_ERR_TLS_NO_TIME:
        return "Couldn't get the correct time (NTP server unreachable and the console's clock is "
               "wrong)";
    case XC_ERR_TLS_NO_RANDOM:
        return "Couldn't gather enough randomness for a secure connection";
    case XC_ERR_TLS_NOT_TLS:
        return "The server didn't answer in HTTPS (is it a plain http:// server?)";
    case XC_ERR_IMAGE_HASH:
        return "The downloaded image doesn't match its published checksum";
    case XC_ERR_NTP:
        return "The time server's answer was refused (not synchronized, rate limit or invalid)";
    case XC_ERR_COUNT_:
        break;
    }
    return "Unknown error";
}
