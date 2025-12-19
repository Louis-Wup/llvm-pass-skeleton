int main() {
    int a = 5;
    int b = 10;
    int c = a + b;
    int d = c * 2;
    int e = d + 5; // unused
    
    int x = 100; // dead store
    x = 200;
    
    return x;
}
