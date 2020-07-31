#include <iostream>
#include "swift/Basic/Version.h"

int main(int argc, char* argv[]) {
    swift::version::getSwiftNumericVersion();
    std::cout << swift::version::getSwiftFullVersion() << std::endl;
    return 0;
}
