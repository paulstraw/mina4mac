// winlist <pid> [title prefix]: print "id WxH title" for each window owned by pid (CGWindowList), or only
// those whose title starts with the prefix. Used by tools/screenshot.sh to find the game's window.
#include <CoreGraphics/CoreGraphics.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: winlist <pid> [title prefix]\n"); return 2; }
    int pid = atoi(argv[1]);
    CFArrayRef a = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
    for (CFIndex i = 0; i < CFArrayGetCount(a); i++) {
        CFDictionaryRef d = CFArrayGetValueAtIndex(a, i);
        int p = 0, id = 0;
        CFNumberGetValue(CFDictionaryGetValue(d, kCGWindowOwnerPID), kCFNumberIntType, &p);
        if (p != pid) continue;
        CFNumberGetValue(CFDictionaryGetValue(d, kCGWindowNumber), kCFNumberIntType, &id);
        CGRect r;
        CGRectMakeWithDictionaryRepresentation(CFDictionaryGetValue(d, kCGWindowBounds), &r);
        char name[512] = "";
        CFStringRef n = CFDictionaryGetValue(d, kCGWindowName);
        if (n) CFStringGetCString(n, name, sizeof name, kCFStringEncodingUTF8);
        if (argc > 2 && strncmp(name, argv[2], strlen(argv[2]))) continue;
        printf("%d %gx%g %s\n", id, r.size.width, r.size.height, name);
    }
    CFRelease(a);
    return 0;
}
