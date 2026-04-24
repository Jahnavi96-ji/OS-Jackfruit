#include <stdio.h>
#include <unistd.h>
int main() {
    int i = 0;
    while (1) {
        printf("running %d\n", i++);
        fflush(stdout);
        sleep(2);
    }
    return 0;
}
