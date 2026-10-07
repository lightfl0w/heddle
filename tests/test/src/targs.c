#include <stdio.h>
int main(int argc, char **argv) {
    printf("argc=%d", argc);
    for (int i = 1; i < argc; i++) printf(" [%s]", argv[i]);
    printf("\n");
    return argc == 3 ? 0 : 1;
}
