#include <iostream>

int max(int a, int b) {
    return (a > b) ? a : b;
}

int inc(int x) {
    return x + 1;
}

int main() {
    int a = 9;
    int b = -4;
    int m = max(a, b);
    int x = abs(b);
    int y = inc(m);
    int z = y + x;
    std::cout << z << std::endl;
    return 0;
}