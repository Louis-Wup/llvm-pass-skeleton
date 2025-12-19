int main(int argc, char** argv) {
    int a = argc;
    int b = argc + 10;
    int c = a * b;
    int d = a + b;
    int e = a * b; // Redundant with c
    int f = b + a; // Redundant with d (commutative)
    int g = c + d;
    int h = e + f; // Redundant with g
    return h;
}
