#include <zlib.h>
int main(void) { return zlibVersion() ? 0 : 1; }
