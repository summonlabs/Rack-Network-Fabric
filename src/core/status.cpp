// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include "rnf/core/status.hpp"

#include "rnf/core/id.hpp"

namespace rnf {

std::string Status::describe() const {
    std::string out(to_string(code_));
    if (!detail_.empty()) {
        out += ": ";
        out += detail_;
    }
    return out;
}

template class Result<std::uint64_t>;
template class Result<std::int64_t>;
template class Result<bool>;
template class Result<std::string>;
template class Result<Digest>;

}  // namespace rnf
