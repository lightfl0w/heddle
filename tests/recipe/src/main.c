#include <stdio.h>

#include "greet.h"

int main(void) {
    printf("%d\n", greet());
    return greet() == 102 ? 0 : 1;
}
