//
// Created by P21_0044 on 31/07/20.
//

#include "llvm/Support/raw_ostream.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Version.h"

#include <vector>

namespace swift {
    namespace version {

        std::pair<unsigned, unsigned> getSwiftNumericVersion() {
            return {3, 1};
        }

        std::string getSwiftFullVersion() {
            std::string buf;
            llvm::raw_string_ostream OS(buf);
            OS << "Swift version 3.1.1";
            return OS.str();
        }
    } // end namespace version
} // end namespace swift
