///// LLVM analysis pass to mitigate false sharing based on profiling data /////
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>
#include <set>
#include <vector>

using namespace llvm;

// TODO: This is a hack
static bool isLocalToModule(StructType *type) {
  return type->hasName() && type->getName().contains("(anonymous namespace)");
}

namespace {
// TODO: Do we want a priority here?
// Right now, the pass realigns all falsely shared data.
// That might add a lot of padding.
struct CacheLineEntry {
  std::string variableName;
  size_t accessOffsetInVariable; // The offset of the access within this variable, in bytes.
  size_t accessSize;             // The size of the read/write, in bytes.
};
}

namespace{
// Represents a pair of memory locations that were accessed by different CPUs
// but resided on the same cache line during profiling.
struct Conflict {
  CacheLineEntry entry1;
  CacheLineEntry entry2;
  uint64_t priority;
};
}

std::istream &operator>>(std::istream &in, CacheLineEntry &entry) {
  return in >> entry.variableName >> entry.accessOffsetInVariable >> entry.accessSize;
}
std::istream &operator>>(std::istream &in, Conflict &conflict) {
  return in >> conflict.entry1 >> conflict.entry2 >> conflict.priority;
}

namespace{
struct Fix583 : public ModulePass {
  static char ID;

  // TODO: Come up with a better way of taking input.
  // Maybe consider doing something similar to profiling passes.
  static const std::string inputFile;

  // TODO: Is this accurate?
  // Also, make sure this is a power of 2.
  static const size_t cacheLineSize = 64; // in bytes

  std::vector<Conflict> getPotentialFS() {
    std::ifstream in(inputFile);

    std::vector<Conflict> conflicts;
    Conflict conflict;
    while (in >> conflict) {
      conflicts.push_back(std::move(conflict));
    }

    return conflicts;
  }

  Fix583() : ModulePass(ID) {}

  bool runOnModule(Module &M) override {
    bool changed = false;
    auto conflicts = getPotentialFS();
    std::unordered_map<std::string, std::set<size_t>> structAccesses;
    for (auto &conflict : conflicts) {
      auto *global1 = M.getGlobalVariable(conflict.entry1.variableName, true);
      auto *global2 = M.getGlobalVariable(conflict.entry2.variableName, true);
      if (!global1 || !global2) {
        continue;
      }
      if (conflict.entry1.variableName == conflict.entry2.variableName) {
        if (auto *type = dyn_cast<StructType>(global1->getValueType())) {
          if (isLocalToModule(type) && type->hasName() && !type->isPacked()) {
            auto &set = structAccesses[std::string(type->getName())];
            set.insert(conflict.entry1.accessOffsetInVariable);
            set.insert(conflict.entry2.accessOffsetInVariable);
          }
        }
      } else {
        // TODO: Can this be more efficient?
        global1->setAlignment(Align(cacheLineSize));
        global2->setAlignment(Align(cacheLineSize));
        changed = true;
      }
    }

    auto &dataLayout = M.getDataLayout();
    for (auto &pair : structAccesses) {
      auto *type = StructType::getTypeByName(M.getContext(), pair.first);
      assert(type != nullptr);
      auto *layout = dataLayout.getStructLayout(type);
      std::set<unsigned int> conflictingElements;
      for (size_t offset : pair.second) {
        conflictingElements.insert(layout->getElementContainingOffset(offset));
      }
      // Create a new, padded struct type.
      // Record a map from (old member offset) => (new member offset).
      if (conflictingElements.size() > 1) {
        std::vector<Type *> types(type->element_begin(), type->element_end());
        for (unsigned int element : conflictingElements) {
          // TODO
        }
        // Rewrite all old struct uses to use the new struct type.
        // Need to fix the following instruction types:
        //  - extractvalue
        //  - insertvalue
        //  - alloca
        //  - getelementptr
        // Also, need to fix:
        //  - Other structs/arrays that contains this struct type
        //  - Function arguments
        //  - Global variables
        changed = true;
      }
    }
    return changed;
  }
}; // end of struct Fix583
}  // end of anonymous namespace

char Fix583::ID = 0;
const std::string Fix583::inputFile = "fs_conflicts.txt";
static RegisterPass<Fix583> X("false-sharing-fix", "Pass to fix false sharing",
                              false /* Only looks at CFG */,
                              false /* Analysis Pass */);