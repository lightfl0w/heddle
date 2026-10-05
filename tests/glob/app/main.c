#include <stdio.h>

#include "add.h"

int extra_a(void);
int extra_b(void);

int main(void) {
    printf("%d\n", add(20, 21) + extra_a() + extra_b());
    return 0;
}
