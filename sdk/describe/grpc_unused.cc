// The embedded generators only describe Card messages, which have no RPCs.
// Keep their optional gRPC entry points out of the native host's dependencies.
#include <flatbuffers/idl.h>

#include <stdexcept>
namespace flatbuffers {
auto TsugumiDescribeEnsureDirExists(const std::string&) -> void {}
auto LogCompilerError(const std::string& error) -> void {
  throw std::runtime_error(error);
}
auto GenerateCppGRPC(const Parser&, const std::string&, const std::string&)
    -> bool {
  return false;
}
auto GeneratePythonGRPC(const Parser&, const std::string&, const std::string&)
    -> bool {
  return false;
}
auto GenerateGoGRPC(const Parser&, const std::string&, const std::string&)
    -> bool {
  return false;
}
}  // namespace flatbuffers
