/* notify: shows a notification on the desktop: notify <text...> */
#include <stdio.h>
#include <string.h>
#include <vexa/gui.h>

int main(int argc, char **argv) {
    char text[100] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strncat(text, " ", sizeof(text) - strlen(text) - 1);
        }
        strncat(text, argv[i], sizeof(text) - strlen(text) - 1);
    }
    if (!text[0]) {
        fprintf(stderr, "usage: notify <text>\n");
        return 1;
    }
    vx_notify(text);
    return 0;
}
