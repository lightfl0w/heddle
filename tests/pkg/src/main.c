#include <stdio.h>

#include "freertos.h"

int main(void) {
    printf("%d\n", freertos_version());

#ifdef USING_MANAGED_TOOLCHAIN
    printf("managed\n");
#else
    printf("host\n");
#endif

    return 0;
}
