/**
 * @file main.c
 * @brief Entry point for camera-server, the network security camera daemon.
 *
 * Placeholder for Sprint 1: prints the version. Capture, streaming and recording are added
 * in Sprint 2 and 3.
 */

#include <stdio.h>
#include <string.h>

static void usage(const char *prog)
{
    printf("Usage: %s [--version]\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("camera-server %s\n", VERSION);
        return 0;
    }

    if (argc > 1) {
        usage(argv[0]);
        return 1;
    }

    printf("camera-server %s: not implemented yet\n", VERSION);
    return 0;
}
