// Output type resolution for Collect Cards, independent of run values.
#pragma once

#include <string>

#include "tsugumi/document/types.hh"

namespace tsugumi {

// The output type of a Collect Card whose input port is `source`. FlatBuffer
// members are collected into the core-owned `tsugumi.builtin.Collected
// { items:[Tsugumi.Card.CardPort] }`, one opaque envelope per member. Path
// members are collected into one directory, a Path output whose type is not
// used. Returns null with `error` set when the type cannot be built.
auto collect_output_type(const types::PortType& source, std::string& error)
    -> types::TypeRef;
auto collected_type() -> types::TypeRef;

}  // namespace tsugumi
