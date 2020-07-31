#include <iostream>
#include "swift/Basic/Version.h"

int main() {
    swift::version::getSwiftNumericVersion();
    std::cout << swift::version::getSwiftFullVersion() << std::endl;
    return 0;
}
