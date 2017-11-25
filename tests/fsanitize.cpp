void test_fsanitize_function()
    {
    int* arr = new int[10];
    delete[] arr;
    int r = arr[ 0 ];
    (void)r;
    }

int main()
    {
    test_fsanitize_function();
    return 0;
    }
