//===--- Version.cpp - Swift Version Number -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines several version-related utility functions for Swift.
//
//===----------------------------------------------------------------------===//

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
