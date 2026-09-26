// Undecorate each line of stdin (tools/undnametest.py): prints the result, or "?" if unsupported.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "undname.h"

int main(void) {
    char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        line[strcspn(line, "\n")] = 0;
        char *u = undname_type(line);
        puts(u ? u : "?");
        free(u);
    }
    return 0;
}
