/* test tool: probe_track on each FILE, as the estimate calls it; one line per file on stdout:
 * NAME \t ok \t SECONDS \t AAC   or   NAME \t refused \t WHY */
#include <stdio.h>
#include <string.h>
#include "../rbsp.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *s = strrchr(argv[i], '/'), *name = s ? s + 1 : argv[i];
        double sec = 0;
        int aac = 0;
        char why[300];
        if (probe_track(argv[i], &sec, &aac, why, sizeof why)) printf("%s\trefused\t%s\n", name, why);
        else printf("%s\tok\t%.6f\t%d\n", name, sec, aac);
    }
    return 0;
}
