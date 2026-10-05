#include "greet.h"

int greet(void) {
    return GREET_LEVEL + greet_asm();
}
