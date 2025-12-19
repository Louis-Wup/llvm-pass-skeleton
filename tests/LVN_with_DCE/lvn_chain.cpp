int main(int argc, char** argv) {
    int a = argc;
    int b = argc + 10;
    int c = argc + 20;
    int d = a + b;
    int e = d + c;
    int f = (a + b) + c; // Should be redundant with e
    return f;
}
