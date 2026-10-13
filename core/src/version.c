#include "xcat/version.h"

#define XC_STR_(x) #x
#define XC_STR(x) XC_STR_(x)

const char *xc_version_string(void)
{
    return XC_STR(XC_VERSION_MAJOR) "." XC_STR(XC_VERSION_MINOR) "." XC_STR(XC_VERSION_PATCH);
}

uint32_t xc_version_number(void)
{
    return ((uint32_t)XC_VERSION_MAJOR << 16U) | ((uint32_t)XC_VERSION_MINOR << 8U) |
           (uint32_t)XC_VERSION_PATCH;
}
