#include <iostream>
#include "Source/native/win_input.h"

int main() {
    std::cout << "Starting to type in 3 seconds..." << std::endl;
    Sleep(3000);
    WinInput::typeUtf8("Hello world test it works! ");
    std::cout << "Done typing." << std::endl;
    return 0;
}
