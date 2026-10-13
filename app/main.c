/*
 * xiso-cartographer XBE entry point.
 *
 * M0 "hello" build: proves the nxdk toolchain works and that libxcat (core/)
 * compiles and runs on the Xbox. Replaced by the real UI in M8.
 */
#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>

#include "xcat/version.h"

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("xiso-cartographer %s\n", xc_version_string());
    debugPrint("libxcat version number: 0x%06lx\n", (unsigned long)xc_version_number());
    debugPrint("\nHello from nxdk. Toolchain OK.\n");

    for (;;) {
        Sleep(1000);
    }
}
