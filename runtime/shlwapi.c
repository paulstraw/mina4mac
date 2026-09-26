// SHLWAPI implemented natively (HLE).
#include "host.h"

enum { MAX_PATH = 260 };

// PathAppendW(path[MAX_PATH], more): strip more's leading backslashes, join with one backslash, then
// canonicalise like PathCanonicalize ("." components dropped, ".." removes the previous one).
HOST_STDCALL(shlwapi, PathAppendW, 8) {
    uint32_t path = ARG(0), more = ARG(1);
    uint16_t buf[2 * MAX_PATH + 2];
    uint32_t n = 0;
    while (rd16(path + 2 * n) && n < MAX_PATH) buf[n] = rd16(path + 2 * n), n++;
    while (rd16(more) == '\\') more += 2;
    if (n && buf[n - 1] != '\\' && rd16(more)) buf[n++] = '\\';
    for (; rd16(more) && n < 2 * MAX_PATH; more += 2) buf[n++] = rd16(more);
    buf[n] = 0;
    // Canonicalise in place, component by component.
    uint32_t o = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i;
        while (j < n && buf[j] != '\\') j++;
        uint32_t len = j - i;
        int dot = len == 1 && buf[i] == '.', dotdot = len == 2 && buf[i] == '.' && buf[i + 1] == '.';
        if (dotdot && o) {  // drop the previous component (not a drive "X:")
            uint32_t k = o - 1;
            while (k && buf[k - 1] != '\\') k--;
            if (!(o - k == 3 && buf[k + 1] == ':')) o = k;
        } else if (!dot) {
            for (uint32_t k = i; k < j; k++) buf[o++] = buf[k];
            if (j < n) buf[o++] = '\\';
        }
        i = j + 1;
    }
    if (o >= MAX_PATH) return ret_i32(c, 0);
    for (uint32_t k = 0; k < o; k++) wr16(path + 2 * k, buf[k]);
    wr16(path + 2 * o, 0);
    ret_i32(c, 1);
}
