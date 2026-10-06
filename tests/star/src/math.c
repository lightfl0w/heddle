#include "util.h"

int add(int a, int b) {
    return a + b + util_id() - 40;
}
