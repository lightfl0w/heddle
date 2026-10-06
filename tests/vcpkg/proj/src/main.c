#include <stdio.h>

#include "zlib.h"

int main(void) {
    printf("%d\n", zlib_version());
    return 0;
}
