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

        Version Version::asMajorVersion() const {
            if (empty())
                return {};
            Version res;
            res.Components.push_back(Components[0]);
            return res;
        }

        bool operator>=(const class Version &lhs,
                        const class Version &rhs) {

            // The empty compiler version represents the latest possible version,
            // usually built from the source repository.
            if (lhs.empty())
                return true;

            auto n = std::max(lhs.size(), rhs.size());

            for (size_t i = 0; i < n; ++i) {
                auto lv = i < lhs.size() ? lhs[i] : 0;
                auto rv = i < rhs.size() ? rhs[i] : 0;
                if (lv < rv)
                    return false;
                else if (lv > rv)
                    return true;
            }
            // Equality
            return true;
        }

        bool operator==(const class Version &lhs,
                        const class Version &rhs) {
            auto n = std::max(lhs.size(), rhs.size());
            for (size_t i = 0; i < n; ++i) {
                auto lv = i < lhs.size() ? lhs[i] : 0;
                auto rv = i < rhs.size() ? rhs[i] : 0;
                if (lv != rv)
                    return false;
            }
            return true;
        }

        std::pair<unsigned, unsigned> getSwiftNumericVersion() {
            return {3, 1};
        }

        std::string getSwiftFullVersion(Version effectiveVersion) {
            std::string buf;
            llvm::raw_string_ostream OS(buf);
            OS << "Swift version (swift-3.1.1-RELEASE)";
            return OS.str();
        }
    } // end namespace version
} // end namespace swift
