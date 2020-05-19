#pragma once

#include <vector>

namespace llvm {
  class Function;
  class CallSite;
} // end namespace llvm

namespace previrt {
namespace transforms {

  void promoteCallWithMultipleCallees(llvm::CallSite &CS,
				      const std::vector<llvm::Function*> &Callees);

} // end transforms
} // end previrt
