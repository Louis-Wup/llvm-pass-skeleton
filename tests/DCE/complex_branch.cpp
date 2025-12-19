int g = 0;

int complex_branch(int a) {
    int x = 0;
    if (a > 0) {
        x = 10; // Dead store
        g = 1;  // Global store (should be kept)
    } else {
        x = 20; // Dead store
    }
    return 0;
}

int main() {
    complex_branch(1);
    return g; // Return global variable to verify side effect
}
