int complex_loop() {
    int sum = 0;
    int dead_counter = 0;
    for (int i = 0; i < 10; i++) {
        sum += i;
        dead_counter++; // Dead store/computation
    }
    return sum;
}

int main() {
    return complex_loop();
}
