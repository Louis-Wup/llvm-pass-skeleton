int main() {
    int a = 10;
    int b = 20;
    int c = a * b;
    int d = a * b; // redundant
    int e = c + d;
    return e;
}
