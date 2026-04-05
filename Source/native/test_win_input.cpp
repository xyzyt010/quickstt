#include "win_input.h"
#include <iostream>

int main() {
    std::cout << "Waiting 3 seconds..." << std::endl;
    Sleep(3000);
    std::cout << "Typing..." << std::endl;
    WinInput::typeUtf8("Hello from win_input.h!");
    WinInput::tryCommand("enter");
    std::cout << "Done!" << std::endl;
    return 0;
}
