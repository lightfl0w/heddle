#include <stdio.h>
int add(int, int);
int main(void) {
    printf("sum=%d\n", add(2, 3));
    return add(2, 3) == 5 ? 0 : 1;
}
