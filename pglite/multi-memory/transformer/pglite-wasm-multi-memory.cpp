/*
 * Copyright 2026 Electric DB Limited
 * SPDX-License-Identifier: Apache-2.0
 *
 * Correctness-first two-domain WebAssembly memory transformer for PGlite.
 *
 * This tool is intentionally built against the exact Binaryen revision in the
 * pinned Emscripten SDK. It accepts a conventional, imported-memory wasm32
 * module and adds a second imported memory for cluster-global pointers. Every
 * dereferencing instruction in defined input functions is replaced by a call
 * to a deduplicated per-shape helper. The helper validates pointer tags and
 * aperture bounds before selecting private memory 0 or global memory 1.
 */

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ir/find_all.h"
#include "ir/local-graph.h"
#include "pass.h"
#include "wasm-builder.h"
#include "wasm-features.h"
#include "wasm-io.h"
#include "wasm-traversal.h"
#include "wasm-validator.h"
#include "wasm.h"

using namespace wasm;

namespace {

constexpr const char* ToolVersion = "0.5.0";
constexpr const char* PointerABI = "pglite-tagged-i32-v1";
constexpr const char* ABISectionName = "pglite.multi-memory.abi";
constexpr const char* HelperPrefix = "__pglite_mm_";
constexpr uint64_t PrivateAperture = uint64_t(1) << 31;
constexpr uint64_t GlobalAperture = uint64_t(1) << 30;

struct Options {
  std::string input;
  std::string output;
  std::string report;
  std::string inputSourceMap;
  std::string outputSourceMap;
  std::string outputSourceMapURL;
  std::string inputSHA256;
  std::string globalImportModule = "pglite";
  std::string globalImportBase = "global_memory";
  std::string scopedImportBase = "scoped_memory";
  std::vector<std::string> inputFeatures;
  std::vector<std::string> privateReturnExports;
  std::vector<uint64_t> directPrivateFunctionIndices;
  uint64_t globalInitialPages = UINT64_MAX;
  uint64_t globalMaximumPages = UINT64_MAX;
  bool inlinePrivateFastPath = false;
  bool privateOnlyOracle = false;
  bool profileFunctionEntries = false;
  bool provenance = false;
  bool emitText = false;
};

enum class HelperKind {
  Load,
  Store,
  AtomicRMW,
  AtomicCmpxchg,
  AtomicWait,
  AtomicNotify,
  SIMDLoad,
  SIMDLoadStoreLane,
  MemoryCopy,
  MemoryFill,
};

struct HelperSpec {
  HelperKind kind = HelperKind::Load;
  uint8_t bytes = 0;
  bool signed_ = false;
  bool atomic = false;
  uint64_t offset = 0;
  uint64_t align = 0;
  Type type = Type::none;
  Type valueType = Type::none;
  AtomicRMWOp rmwOp = RMWAdd;
  SIMDLoadOp simdLoadOp = Load8SplatVec128;
  SIMDLoadStoreLaneOp simdLaneOp = Load8LaneVec128;
  uint8_t lane = 0;
  bool laneStore = false;
};

std::string kindName(HelperKind kind) {
  switch (kind) {
    case HelperKind::Load:
      return "load";
    case HelperKind::Store:
      return "store";
    case HelperKind::AtomicRMW:
      return "atomic-rmw";
    case HelperKind::AtomicCmpxchg:
      return "atomic-cmpxchg";
    case HelperKind::AtomicWait:
      return "atomic-wait";
    case HelperKind::AtomicNotify:
      return "atomic-notify";
    case HelperKind::SIMDLoad:
      return "simd-load";
    case HelperKind::SIMDLoadStoreLane:
      return "simd-lane";
    case HelperKind::MemoryCopy:
      return "memory-copy";
    case HelperKind::MemoryFill:
      return "memory-fill";
  }
  throw std::runtime_error("unknown helper kind");
}

std::string typeName(Type type) {
  std::ostringstream out;
  out << type;
  return out.str();
}

std::string helperKey(const HelperSpec& spec) {
  std::ostringstream out;
  out << int(spec.kind) << ':' << int(spec.bytes) << ':' << spec.signed_ << ':'
      << spec.atomic << ':' << spec.offset << ':' << spec.align << ':'
      << typeName(spec.type) << ':' << typeName(spec.valueType) << ':'
      << int(spec.rmwOp) << ':' << int(spec.simdLoadOp) << ':'
      << int(spec.simdLaneOp) << ':' << int(spec.lane) << ':' << spec.laneStore;
  return out.str();
}

std::string jsonEscape(const std::string& input) {
  std::ostringstream out;
  for (unsigned char c : input) {
    switch (c) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (c < 0x20) {
          out << "\\u";
          const char* hex = "0123456789abcdef";
          out << '0' << '0' << hex[c >> 4] << hex[c & 15];
        } else {
          out << c;
        }
    }
  }
  return out.str();
}

uint64_t parseUnsigned(const std::string& value, const char* option) {
  size_t used = 0;
  uint64_t result = 0;
  try {
    result = std::stoull(value, &used, 0);
  } catch (...) {
    throw std::runtime_error(std::string("invalid value for ") + option +
                             ": " + value);
  }
  if (used != value.size()) {
    throw std::runtime_error(std::string("invalid value for ") + option +
                             ": " + value);
  }
  return result;
}

void usage(std::ostream& out) {
  out << "Usage: pglite-wasm-multi-memory [options] INPUT\n"
      << "\n"
      << "Options:\n"
      << "  -o, --output FILE                 output wasm (required)\n"
      << "      --report FILE                 write JSON transformation report\n"
      << "      --input-source-map FILE       consume input source map\n"
      << "      --output-source-map FILE      emit output source map\n"
      << "      --output-source-map-url URL   sourceMappingURL custom section\n"
      << "      --input-sha256 HEX            record input hash in ABI metadata\n"
      << "      --global-import-module NAME   import module (default pglite)\n"
      << "      --global-import-base NAME     import name (default global_memory)\n"
      << "      --scoped-import-base NAME     reserved import (default scoped_memory)\n"
      << "      --enable-feature NAME         enable a missing input target feature\n"
      << "      --global-initial-pages N      default: private memory initial\n"
      << "      --global-maximum-pages N      default: private memory maximum\n"
      << "      --inline-private-fast-path    direct memory-0 arm at each site\n"
      << "      --private-only-oracle         retain direct memory-0 operations\n"
      << "      --profile-function-entries    import a profiling entry hook\n"
      << "      --direct-private-function-index N  diagnostic direct function\n"
      << "      --provenance                  enable sound direct-access proofs\n"
      << "      --private-return-export NAME  provenance summary (repeatable)\n"
      << "  -S, --emit-text                   emit WAT instead of binary\n"
      << "  -h, --help                        show this help\n"
      << "      --version                     show tool version\n";
}

Options parseOptions(int argc, const char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto take = [&](const char* option) -> std::string {
      if (++i >= argc) {
        throw std::runtime_error(std::string("missing value for ") + option);
      }
      return argv[i];
    };

    if (arg == "-h" || arg == "--help") {
      usage(std::cout);
      std::exit(0);
    } else if (arg == "--version") {
      std::cout << "pglite-wasm-multi-memory " << ToolVersion << '\n';
      std::exit(0);
    } else if (arg == "-o" || arg == "--output") {
      options.output = take(arg.c_str());
    } else if (arg == "--report") {
      options.report = take(arg.c_str());
    } else if (arg == "--input-source-map") {
      options.inputSourceMap = take(arg.c_str());
    } else if (arg == "--output-source-map") {
      options.outputSourceMap = take(arg.c_str());
    } else if (arg == "--output-source-map-url") {
      options.outputSourceMapURL = take(arg.c_str());
    } else if (arg == "--input-sha256") {
      options.inputSHA256 = take(arg.c_str());
    } else if (arg == "--global-import-module") {
      options.globalImportModule = take(arg.c_str());
    } else if (arg == "--global-import-base") {
      options.globalImportBase = take(arg.c_str());
    } else if (arg == "--scoped-import-base") {
      options.scopedImportBase = take(arg.c_str());
    } else if (arg == "--enable-feature") {
      options.inputFeatures.push_back(take(arg.c_str()));
    } else if (arg == "--global-initial-pages") {
      options.globalInitialPages = parseUnsigned(take(arg.c_str()), arg.c_str());
    } else if (arg == "--global-maximum-pages") {
      options.globalMaximumPages = parseUnsigned(take(arg.c_str()), arg.c_str());
    } else if (arg == "--inline-private-fast-path") {
      options.inlinePrivateFastPath = true;
    } else if (arg == "--private-only-oracle") {
      options.privateOnlyOracle = true;
    } else if (arg == "--profile-function-entries") {
      options.profileFunctionEntries = true;
    } else if (arg == "--direct-private-function-index") {
      options.directPrivateFunctionIndices.push_back(
        parseUnsigned(take(arg.c_str()), arg.c_str()));
    } else if (arg == "--provenance") {
      options.provenance = true;
    } else if (arg == "--private-return-export") {
      options.privateReturnExports.push_back(take(arg.c_str()));
    } else if (arg == "-S" || arg == "--emit-text") {
      options.emitText = true;
    } else if (!arg.empty() && arg[0] == '-') {
      throw std::runtime_error("unknown option: " + arg);
    } else if (options.input.empty()) {
      options.input = arg;
    } else {
      throw std::runtime_error("unexpected positional argument: " + arg);
    }
  }
  if (options.input.empty()) {
    throw std::runtime_error("input file is required");
  }
  if (options.output.empty()) {
    throw std::runtime_error("--output is required");
  }
  if (options.inlinePrivateFastPath && options.privateOnlyOracle) {
    throw std::runtime_error(
      "--inline-private-fast-path and --private-only-oracle are exclusive");
  }
  if (options.profileFunctionEntries && !options.privateOnlyOracle) {
    throw std::runtime_error(
      "--profile-function-entries requires --private-only-oracle");
  }
  if (options.privateOnlyOracle &&
      !options.directPrivateFunctionIndices.empty()) {
    throw std::runtime_error(
      "--private-only-oracle and --direct-private-function-index are exclusive");
  }
  if (options.provenance &&
      (options.privateOnlyOracle || options.inlinePrivateFastPath ||
       !options.directPrivateFunctionIndices.empty())) {
    throw std::runtime_error(
      "--provenance is exclusive with diagnostic and inline profiles");
  }
  if (!options.privateReturnExports.empty() && !options.provenance) {
    throw std::runtime_error(
      "--private-return-export requires --provenance");
  }
  return options;
}

class Transformer;

class Rewriter : public ExpressionStackWalker<Rewriter> {
  enum class Provenance { Null, Private, Global, Scoped, Unknown };

  Transformer& transformer;
  Module& module;
  Function& function;
  LocalGraph localGraph;
  std::map<std::string, Index> operandTemps;
  std::unordered_map<Expression*, Provenance> provenanceCache;
  std::unordered_set<Expression*> provenanceActive;

  void requirePrivate(Name memory, const char* operation);
  Index memoryNestingDepth();
  Index getOperandTemp(Index depth, Index position, Type type);
  Provenance classify(Expression* expression);
  bool provesPrivate(const HelperSpec& spec,
                     const std::vector<Expression*>& operands);
  void replaceWithHelper(Expression* original,
                         const HelperSpec& spec,
                         std::vector<Expression*> operands,
                         Type result,
                         const char* operationName);

public:
  Rewriter(Transformer& transformer, Module& module, Function& function)
    : transformer(transformer), module(module), function(function),
      localGraph(&function, &module) {}

  bool expressionIsPrivate(Expression* expression) {
    return classify(expression) == Provenance::Private;
  }

  void visitLoad(Load* curr);
  void visitStore(Store* curr);
  void visitAtomicRMW(AtomicRMW* curr);
  void visitAtomicCmpxchg(AtomicCmpxchg* curr);
  void visitAtomicWait(AtomicWait* curr);
  void visitAtomicNotify(AtomicNotify* curr);
  void visitAtomicFence(AtomicFence* curr);
  void visitSIMDLoad(SIMDLoad* curr);
  void visitSIMDLoadStoreLane(SIMDLoadStoreLane* curr);
  void visitMemoryCopy(MemoryCopy* curr);
  void visitMemoryFill(MemoryFill* curr);
  void visitMemoryInit(MemoryInit* curr);
  void visitMemorySize(MemorySize* curr);
  void visitMemoryGrow(MemoryGrow* curr);
  void visitDataDrop(DataDrop* curr);
};

class Transformer {
  struct FunctionStat {
    uint64_t wasmFunctionIndex = 0;
    uint64_t expressionCount = 0;
    uint64_t expressionShapeHash = 1469598103934665603ULL;
    std::map<std::string, uint64_t> operations;
    std::map<std::string, uint64_t> directPrivateOperations;
    std::map<std::string, uint64_t> genericOperations;
  };

  Module& module;
  const Options& options;
  Name privateMemory;
  Name globalMemory;
  Name scopedMemory;
  std::map<std::string, Name> helperNames;
  std::vector<std::pair<Name, HelperSpec>> helperSpecs;
  std::map<std::string, uint64_t> rewritten;
  std::map<std::string, uint64_t> directPrivate;
  std::map<std::string, uint64_t> directPrivateProofs;
  std::map<std::string, uint64_t> allowlisted;
  std::map<std::string, FunctionStat> functionStats;
  Name currentFunction;
  std::unordered_set<Expression*> generatedDirectOperations;
  std::unordered_set<std::string> privateReturnFunctions;
  std::unordered_set<std::string> privateParameters;

  static std::string parameterKey(Name function, Index index) {
    return function.toString() + ":" + std::to_string(index);
  }

  Expression* local(Builder& builder, Index index, Type type) {
    return builder.makeLocalGet(index, type);
  }

  Expression* isGlobal(Builder& builder, Index ptrIndex) {
    return builder.makeBinary(LtSInt32,
                              local(builder, ptrIndex, Type::i32),
                              builder.makeConst(int32_t(0)));
  }

  Expression* maskedGlobalPointer(Builder& builder, Index ptrIndex) {
    return builder.makeBinary(AndInt32,
                              local(builder, ptrIndex, Type::i32),
                              builder.makeConst(uint32_t(0x3fffffff)));
  }

  Expression* rawPointer(Builder& builder, Index ptrIndex) {
    return local(builder, ptrIndex, Type::i32);
  }

  Expression* nullTrap(Builder& builder, Index ptrIndex) {
    return builder.makeIf(
      builder.makeUnary(EqZInt32, local(builder, ptrIndex, Type::i32)),
      builder.makeUnreachable());
  }

  Expression* reservedTagTrap(Builder& builder, Index ptrIndex) {
    auto* tag = builder.makeBinary(
      AndInt32,
      local(builder, ptrIndex, Type::i32),
      builder.makeConst(uint32_t(0xc0000000)));
    return builder.makeIf(
      builder.makeBinary(EqInt32, tag, builder.makeConst(uint32_t(0xc0000000))),
      builder.makeUnreachable());
  }

  Expression* fixedRangeTrap(Builder& builder,
                             Expression* address,
                             uint64_t aperture,
                             uint64_t offset,
                             uint64_t bytes) {
    if (offset > aperture || bytes > aperture || offset + bytes > aperture) {
      return builder.makeUnreachable();
    }
    uint64_t lastStart = aperture - offset - bytes;
    return builder.makeIf(
      builder.makeBinary(GtUInt32,
                         address,
                         builder.makeConst(uint32_t(lastStart))),
      builder.makeUnreachable());
  }

  Expression* dynamicRangeTrap(Builder& builder,
                               Expression* address,
                               Expression* size,
                               uint64_t aperture) {
    auto* sizeTooLarge = builder.makeBinary(
      GtUInt32, size, builder.makeConst(uint32_t(aperture)));
    auto* startTooLarge = builder.makeBinary(
      GtUInt32,
      address,
      builder.makeBinary(SubInt32,
                         builder.makeConst(uint32_t(aperture)),
                         ExpressionManipulator::copy(size, module)));
    return builder.makeIf(builder.makeBinary(OrInt32, sizeTooLarge, startTooLarge),
                          builder.makeUnreachable());
  }

  Expression* withFixedRange(Builder& builder,
                             Expression* address,
                             uint64_t aperture,
                             uint64_t offset,
                             uint64_t bytes,
                             Expression* operation,
                             Type result) {
    return builder.makeBlock(
      {fixedRangeTrap(builder,
                      ExpressionManipulator::copy(address, module),
                      aperture,
                      offset,
                      bytes),
       operation},
      result);
  }

  Expression* makeDirectOperation(const HelperSpec& spec,
                                  Builder& builder,
                                  Name memory,
                                  std::vector<Expression*> operands,
                                  uint64_t aperture,
                                  bool enforceAperture) {
    Expression* address = operands[0];
    Expression* operation = nullptr;
    Type result = spec.type;
    switch (spec.kind) {
      case HelperKind::Load:
        operation = spec.atomic
                      ? static_cast<Expression*>(builder.makeAtomicLoad(
                          spec.bytes,
                          spec.offset,
                          address,
                          spec.type,
                          memory))
                      : static_cast<Expression*>(builder.makeLoad(spec.bytes,
                                                                  spec.signed_,
                                                                  spec.offset,
                                                                  spec.align,
                                                                  address,
                                                                  spec.type,
                                                                  memory));
        break;
      case HelperKind::Store:
        operation = spec.atomic
                      ? static_cast<Expression*>(builder.makeAtomicStore(
                          spec.bytes,
                          spec.offset,
                          address,
                          operands[1],
                          spec.valueType,
                          memory))
                      : static_cast<Expression*>(builder.makeStore(
                          spec.bytes,
                          spec.offset,
                          spec.align,
                          address,
                          operands[1],
                          spec.valueType,
                          memory));
        result = Type::none;
        break;
      case HelperKind::AtomicRMW:
        operation = builder.makeAtomicRMW(spec.rmwOp,
                                          spec.bytes,
                                          spec.offset,
                                          address,
                                          operands[1],
                                          spec.type,
                                          memory);
        break;
      case HelperKind::AtomicCmpxchg:
        operation = builder.makeAtomicCmpxchg(
          spec.bytes,
          spec.offset,
          address,
          operands[1],
          operands[2],
          spec.type,
          memory);
        break;
      case HelperKind::AtomicWait:
        operation = builder.makeAtomicWait(address,
                                           operands[1],
                                           operands[2],
                                           spec.valueType,
                                           spec.offset,
                                           memory);
        result = Type::i32;
        break;
      case HelperKind::AtomicNotify:
        operation = builder.makeAtomicNotify(address,
                                             operands[1],
                                             spec.offset,
                                             memory);
        result = Type::i32;
        break;
      case HelperKind::SIMDLoad:
        operation = builder.makeSIMDLoad(
          spec.simdLoadOp, spec.offset, spec.align, address, memory);
        result = Type::v128;
        break;
      case HelperKind::SIMDLoadStoreLane:
        operation = builder.makeSIMDLoadStoreLane(
          spec.simdLaneOp,
          spec.offset,
          spec.align,
          spec.lane,
          address,
          operands[1],
          memory);
        result = spec.laneStore ? Type::none : Type::v128;
        break;
      case HelperKind::MemoryCopy:
      case HelperKind::MemoryFill:
        throw std::runtime_error("bulk helper reached scalar builder");
    }
    if (!enforceAperture) {
      return operation;
    }
    return withFixedRange(
      builder, address, aperture, spec.offset, spec.bytes, operation, result);
  }

  Expression* makeSinglePointerBody(const HelperSpec& spec, Builder& builder) {
    auto makeOperands = [&](Expression* address) {
      std::vector<Expression*> operands{address};
      auto params = helperParams(spec);
      for (Index i = 1; i < params.size(); ++i) {
        operands.push_back(local(builder, i, params[i]));
      }
      return operands;
    };
    auto* privateOp = makeDirectOperation(
      spec,
      builder,
      privateMemory,
      makeOperands(rawPointer(builder, 0)),
      PrivateAperture,
      false);
    auto* globalOp = makeDirectOperation(
      spec,
      builder,
      globalMemory,
      makeOperands(maskedGlobalPointer(builder, 0)),
      GlobalAperture,
      true);
    auto* dispatch = builder.makeIf(
      isGlobal(builder, 0), globalOp, privateOp, spec.type);
    return builder.makeBlock(
      {nullTrap(builder, 0), reservedTagTrap(builder, 0), dispatch}, spec.type);
  }

  Expression* copyFor(Builder& builder, bool destGlobal, bool sourceGlobal) {
    auto* dest = destGlobal ? maskedGlobalPointer(builder, 0)
                            : rawPointer(builder, 0);
    auto* source = sourceGlobal ? maskedGlobalPointer(builder, 1)
                                : rawPointer(builder, 1);
    auto* sizeForDest = local(builder, 2, Type::i32);
    auto* sizeForSource = local(builder, 2, Type::i32);
    auto* operation = builder.makeMemoryCopy(
      ExpressionManipulator::copy(dest, module),
      ExpressionManipulator::copy(source, module),
      local(builder, 2, Type::i32),
      destGlobal ? globalMemory : privateMemory,
      sourceGlobal ? globalMemory : privateMemory);
    return builder.makeBlock(
      {dynamicRangeTrap(builder,
                        dest,
                        sizeForDest,
                        destGlobal ? GlobalAperture : PrivateAperture),
       dynamicRangeTrap(builder,
                        source,
                        sizeForSource,
                        sourceGlobal ? GlobalAperture : PrivateAperture),
       operation},
      Type::none);
  }

  Expression* makeCopyBody(Builder& builder) {
    auto* privateSource = builder.makeIf(isGlobal(builder, 1),
                                         copyFor(builder, false, true),
                                         copyFor(builder, false, false));
    auto* globalSource = builder.makeIf(isGlobal(builder, 1),
                                        copyFor(builder, true, true),
                                        copyFor(builder, true, false));
    auto* dispatch =
      builder.makeIf(isGlobal(builder, 0), globalSource, privateSource);
    return builder.makeBlock({nullTrap(builder, 0),
                              reservedTagTrap(builder, 0),
                              nullTrap(builder, 1),
                              reservedTagTrap(builder, 1),
                              dispatch});
  }

  Expression* fillFor(Builder& builder, bool global) {
    auto* dest = global ? maskedGlobalPointer(builder, 0)
                        : rawPointer(builder, 0);
    auto* size = local(builder, 2, Type::i32);
    auto* operation = builder.makeMemoryFill(
      ExpressionManipulator::copy(dest, module),
      local(builder, 1, Type::i32),
      local(builder, 2, Type::i32),
      global ? globalMemory : privateMemory);
    return builder.makeBlock(
      {dynamicRangeTrap(builder,
                        dest,
                        size,
                        global ? GlobalAperture : PrivateAperture),
       operation});
  }

  Expression* makeFillBody(Builder& builder) {
    auto* dispatch = builder.makeIf(
      isGlobal(builder, 0), fillFor(builder, true), fillFor(builder, false));
    return builder.makeBlock(
      {nullTrap(builder, 0), reservedTagTrap(builder, 0), dispatch});
  }

  std::vector<Type> helperParams(const HelperSpec& spec) {
    switch (spec.kind) {
      case HelperKind::Load:
      case HelperKind::SIMDLoad:
        return {Type::i32};
      case HelperKind::Store:
        return {Type::i32, spec.valueType};
      case HelperKind::AtomicRMW:
        return {Type::i32, spec.type};
      case HelperKind::AtomicCmpxchg:
        return {Type::i32, spec.type, spec.type};
      case HelperKind::AtomicWait:
        return {Type::i32, spec.valueType, Type::i64};
      case HelperKind::AtomicNotify:
        return {Type::i32, Type::i32};
      case HelperKind::SIMDLoadStoreLane:
        return {Type::i32, Type::v128};
      case HelperKind::MemoryCopy:
        return {Type::i32, Type::i32, Type::i32};
      case HelperKind::MemoryFill:
        return {Type::i32, Type::i32, Type::i32};
    }
    throw std::runtime_error("unknown helper params");
  }

  Type helperResult(const HelperSpec& spec) {
    switch (spec.kind) {
      case HelperKind::Store:
      case HelperKind::MemoryCopy:
      case HelperKind::MemoryFill:
        return Type::none;
      case HelperKind::AtomicWait:
      case HelperKind::AtomicNotify:
        return Type::i32;
      case HelperKind::SIMDLoad:
        return Type::v128;
      case HelperKind::SIMDLoadStoreLane:
        return spec.laneStore ? Type::none : Type::v128;
      default:
        return spec.type;
    }
  }

  void addHelpers() {
    Builder builder(module);
    for (const auto& [name, spec] : helperSpecs) {
      Expression* body = nullptr;
      if (spec.kind == HelperKind::MemoryCopy) {
        body = makeCopyBody(builder);
      } else if (spec.kind == HelperKind::MemoryFill) {
        body = makeFillBody(builder);
      } else {
        body = makeSinglePointerBody(spec, builder);
      }
      auto params = helperParams(spec);
      auto result = helperResult(spec);
      auto function = Builder::makeFunction(
        name, Signature(Type(params), result), {}, body);
      function->setExplicitName(name);
      function->noFullInline = false;
      function->noPartialInline = true;
      module.addFunction(std::move(function));
    }
  }

  void addGlobalMemory() {
    if (module.memories.size() != 1) {
      throw std::runtime_error(
        "input must contain exactly one conventional memory");
    }
    Memory* privateMem = module.memories[0].get();
    if (!privateMem->imported()) {
      throw std::runtime_error(
        "private memory must be imported so it remains memory index 0");
    }
    if (privateMem->addressType != Type::i32) {
      throw std::runtime_error("memory64 input is not supported");
    }
    if (privateMem->initial.addr > PrivateAperture / Memory::kPageSize) {
      throw std::runtime_error("private memory initial exceeds 2 GiB aperture");
    }
    if (privateMem->hasMax() &&
        privateMem->max.addr > PrivateAperture / Memory::kPageSize) {
      throw std::runtime_error("private memory maximum exceeds 2 GiB aperture");
    }
    if (privateMem->shared && !privateMem->hasMax()) {
      throw std::runtime_error("shared private memory requires a maximum");
    }

    privateMemory = privateMem->name;
    globalMemory = Name("__pglite_global_memory");
    scopedMemory = Name("__pglite_scoped_memory");
    if (module.getMemoryOrNull(globalMemory) ||
        module.getMemoryOrNull(scopedMemory)) {
      throw std::runtime_error("reserved memory name already exists");
    }

    uint64_t initial = options.globalInitialPages == UINT64_MAX
                         ? privateMem->initial.addr
                         : options.globalInitialPages;
    uint64_t maximum = options.globalMaximumPages == UINT64_MAX
                         ? privateMem->max.addr
                         : options.globalMaximumPages;
    if (initial > Memory::kMaxSize32 || maximum > Memory::kMaxSize32 ||
        initial > maximum) {
      throw std::runtime_error("invalid global memory limits");
    }

    auto addMemoryImport = [&](Name name, const std::string& base) {
      auto memory = std::make_unique<Memory>();
      memory->setExplicitName(name);
      memory->module = Name(options.globalImportModule);
      memory->base = Name(base);
      memory->initial = initial;
      memory->max = maximum;
      memory->shared = privateMem->shared;
      memory->addressType = Type::i32;
      module.addMemory(std::move(memory));
    };
    addMemoryImport(globalMemory, options.globalImportBase);
    addMemoryImport(scopedMemory, options.scopedImportBase);
    module.features.setMultiMemory();
    if (privateMem->shared) {
      module.features.setAtomics();
    }
    module.hasFeaturesSection = true;
  }

  void rejectAlreadyTransformed() {
    for (const auto& section : module.customSections) {
      if (section.name == ABISectionName) {
        throw std::runtime_error("module already has PGlite memory ABI metadata");
      }
    }
  }

  void audit() {
    struct Auditor
      : public PostWalker<Auditor, UnifiedExpressionVisitor<Auditor>> {
      Name privateMemory;
      const std::unordered_set<Expression*>& generatedDirectOperations;
      std::vector<std::string> errors;

      Auditor(Name privateMemory,
              const std::unordered_set<Expression*>& generatedDirectOperations)
        : privateMemory(privateMemory),
          generatedDirectOperations(generatedDirectOperations) {}

      void visitExpression(Expression* curr) {
        switch (curr->_id) {
          case Expression::LoadId:
          case Expression::StoreId:
          case Expression::AtomicRMWId:
          case Expression::AtomicCmpxchgId:
          case Expression::AtomicWaitId:
          case Expression::AtomicNotifyId:
          case Expression::SIMDLoadId:
          case Expression::SIMDLoadStoreLaneId:
          case Expression::MemoryCopyId:
          case Expression::MemoryFillId:
            if (generatedDirectOperations.count(curr)) {
              break;
            }
            errors.push_back(std::string("untransformed operation: ") +
                             getExpressionName(curr));
            break;
          case Expression::MemoryInitId:
            if (curr->cast<MemoryInit>()->memory != privateMemory) {
              errors.push_back("memory.init is not private-memory allowlisted");
            }
            break;
          case Expression::MemorySizeId:
            if (curr->cast<MemorySize>()->memory != privateMemory) {
              errors.push_back("memory.size is not private-memory allowlisted");
            }
            break;
          case Expression::MemoryGrowId:
            if (curr->cast<MemoryGrow>()->memory != privateMemory) {
              errors.push_back("memory.grow is not private-memory allowlisted");
            }
            break;
          case Expression::AtomicFenceId:
          case Expression::DataDropId:
            break;
          default:
            break;
        }
      }
    };

    for (const auto& function : module.functions) {
      if (function->imported() ||
          function->name.toString().rfind(HelperPrefix, 0) == 0) {
        continue;
      }
      Auditor auditor(privateMemory, generatedDirectOperations);
      auditor.walkFunctionInModule(function.get(), &module);
      if (!auditor.errors.empty()) {
        std::ostringstream message;
        message << "post-transform memory audit failed in "
                << function->name.toString();
        for (const auto& error : auditor.errors) {
          message << "\n  " << error;
        }
        throw std::runtime_error(message.str());
      }
    }
  }

  std::string manifestJSON() const {
    std::ostringstream out;
    out << '{'
        << "\"schema\":1,"
        << "\"tool\":\"pglite-wasm-multi-memory\","
        << "\"toolVersion\":\"" << ToolVersion << "\","
        << "\"binaryenCommit\":\"52bc45fc34ec6868400216074744147e9d922685\","
        << "\"pointerABI\":\"" << PointerABI << "\","
        << "\"profile\":\""
        << (!options.directPrivateFunctionIndices.empty()
              ? "profile-guided-private-oracle"
              : options.provenance
              ? "two-domain-provenance"
              : options.privateOnlyOracle
              ? (options.profileFunctionEntries
                   ? "private-only-oracle-function-profile"
                   : "private-only-oracle")
              : options.inlinePrivateFastPath
                  ? "two-domain-generic-private-fast-path"
                  : "two-domain-generic")
        << "\","
        << "\"features\":\"" << jsonEscape(module.features.toString()) << "\","
        << "\"featureBits\":" << uint32_t(module.features) << ','
        << "\"privateMemory\":\"" << jsonEscape(privateMemory.toString())
        << "\","
        << "\"globalMemory\":\"" << jsonEscape(globalMemory.toString())
        << "\","
        << "\"scopedMemory\":\"" << jsonEscape(scopedMemory.toString())
        << "\","
        << "\"privateTag\":0,\"globalTag\":2,\"reservedTag\":3,"
        << "\"privateApertureBytes\":" << PrivateAperture << ','
        << "\"globalApertureBytes\":" << GlobalAperture << ','
        << "\"inputSHA256\":\"" << jsonEscape(options.inputSHA256) << "\","
        << "\"helperCount\":" << helperSpecs.size() << '}';
    return out.str();
  }

  void addManifest() {
    auto json = manifestJSON();
    CustomSection section;
    section.name = ABISectionName;
    section.data.assign(json.begin(), json.end());
    module.customSections.push_back(std::move(section));
  }

  void replaceSourceMapURL() {
    module.customSections.erase(
      std::remove_if(module.customSections.begin(),
                     module.customSections.end(),
                     [](const CustomSection& section) {
                       return section.name == "sourceMappingURL";
                     }),
      module.customSections.end());
  }

public:
  Transformer(Module& module, const Options& options)
    : module(module), options(options) {}

  Name getPrivateMemory() const { return privateMemory; }

  bool useInlinePrivateFastPath() const {
    return options.inlinePrivateFastPath;
  }

  bool usePrivateOnlyOracle() const { return options.privateOnlyOracle; }

  bool useProvenance() const { return options.provenance; }

  bool hasPrivateReturn(Name function) const {
    return privateReturnFunctions.count(function.toString());
  }

  bool hasPrivateParameter(Name function, Index index) const {
    return privateParameters.count(parameterKey(function, index));
  }

  bool useCurrentFunctionDirectPrivate() const {
    if (options.directPrivateFunctionIndices.empty()) {
      return false;
    }
    auto index = functionStats.at(currentFunction.toString()).wasmFunctionIndex;
    return std::find(options.directPrivateFunctionIndices.begin(),
                     options.directPrivateFunctionIndices.end(),
                     index) != options.directPrivateFunctionIndices.end();
  }

  void keepPrivateOracleOperation(Expression* operation) {
    generatedDirectOperations.insert(operation);
  }

  std::vector<Type> getHelperParams(const HelperSpec& spec) {
    return helperParams(spec);
  }

  Expression* makeInlinePrivateOperation(const HelperSpec& spec,
                                         Builder& builder,
                                         std::vector<Expression*> operands) {
    auto* operation = makeDirectOperation(spec,
                                          builder,
                                          privateMemory,
                                          std::move(operands),
                                          PrivateAperture,
                                          false);
    generatedDirectOperations.insert(operation);
    return operation;
  }

  Name helperFor(const HelperSpec& spec) {
    auto key = helperKey(spec);
    auto found = helperNames.find(key);
    if (found != helperNames.end()) {
      return found->second;
    }
    auto name = Name(std::string(HelperPrefix) + kindName(spec.kind) + '_' +
                     std::to_string(helperSpecs.size()));
    if (module.getFunctionOrNull(name)) {
      throw std::runtime_error("generated helper name collision");
    }
    helperNames.emplace(key, name);
    helperSpecs.emplace_back(name, spec);
    return name;
  }

  void countAccess(const std::string& name,
                   bool direct,
                   const char* proof = nullptr) {
    auto& stat = functionStats[currentFunction.toString()];
    ++stat.operations[name];
    if (direct) {
      ++directPrivate[name];
      ++stat.directPrivateOperations[name];
      if (proof) {
        ++directPrivateProofs[proof];
      }
    } else {
      ++rewritten[name];
      ++stat.genericOperations[name];
    }
  }
  void countAllowlist(const std::string& name) { ++allowlisted[name]; }

  void initializeFunctionStats() {
    uint64_t importedFunctions = 0;
    for (const auto& function : module.functions) {
      if (function->imported()) {
        ++importedFunctions;
      }
    }
    uint64_t definedOrdinal = 0;
    for (const auto& function : module.functions) {
      if (!function->imported()) {
        auto& stat = functionStats[function->name.toString()];
        stat.wasmFunctionIndex = importedFunctions + definedOrdinal++;
        struct ShapeCounter
          : public PostWalker<ShapeCounter,
                              UnifiedExpressionVisitor<ShapeCounter>> {
          std::map<std::string, uint64_t> kinds;
          void visitExpression(Expression* curr) {
            ++kinds[getExpressionName(curr)];
          }
        } counter;
        counter.walkFunctionInModule(function.get(), &module);
        auto addHash = [&](const std::string& value) {
          for (unsigned char byte : value) {
            stat.expressionShapeHash ^= byte;
            stat.expressionShapeHash *= 1099511628211ULL;
          }
        };
        addHash(function->getParams().toString());
        addHash("->");
        addHash(function->getResults().toString());
        addHash(":" + std::to_string(function->vars.size()));
        for (const auto& [kind, count] : counter.kinds) {
          stat.expressionCount += count;
          addHash(";" + kind + "=" + std::to_string(count));
        }
      }
    }
    for (auto requested : options.directPrivateFunctionIndices) {
      bool found = false;
      for (const auto& [_, stat] : functionStats) {
        found |= stat.wasmFunctionIndex == requested;
      }
      if (!found) {
        throw std::runtime_error("unknown direct-private function index: " +
                                 std::to_string(requested));
      }
    }
  }

  void initializeProvenanceSummaries() {
    for (const auto& exportName : options.privateReturnExports) {
      Export* found = nullptr;
      for (const auto& export_ : module.exports) {
        if (export_->name.toString() == exportName) {
          found = export_.get();
          break;
        }
      }
      if (!found || found->kind != ExternalKind::Function) {
        throw std::runtime_error("private-return export is not a function: " +
                                 exportName);
      }
      auto* function = module.getFunction(found->value);
      if (function->getResults() != Type::i32) {
        throw std::runtime_error("private-return export must return i32: " +
                                 exportName);
      }
      privateReturnFunctions.insert(found->value.toString());
    }
  }

  void inferPrivateParameters() {
    if (!options.provenance) {
      return;
    }
    std::unordered_set<std::string> externallyCallable;
    for (const auto& export_ : module.exports) {
      if (export_->kind == ExternalKind::Function) {
        externallyCallable.insert(export_->value.toString());
      }
    }
    for (const auto& function : module.functions) {
      if (function->imported()) {
        continue;
      }
      for (auto* ref : FindAll<RefFunc>(function->body).list) {
        externallyCallable.insert(ref->func.toString());
      }
    }
    for (const auto& segment : module.elementSegments) {
      for (auto* expression : segment->data) {
        for (auto* ref : FindAll<RefFunc>(expression).list) {
          externallyCallable.insert(ref->func.toString());
        }
      }
    }

    struct CallSite {
      Function* caller;
      Call* call;
    };
    std::unordered_map<std::string, std::vector<CallSite>> callsByTarget;
    for (const auto& caller : module.functions) {
      if (caller->imported()) {
        continue;
      }
      for (auto* call : FindAll<Call>(caller->body).list) {
        callsByTarget[call->target.toString()].push_back(
          {caller.get(), call});
      }
    }

    bool changed;
    do {
      changed = false;
      for (const auto& target : module.functions) {
        auto name = target->name.toString();
        if (target->imported() || externallyCallable.count(name)) {
          continue;
        }
        auto calls = callsByTarget.find(name);
        if (calls == callsByTarget.end() || calls->second.empty()) {
          continue;
        }
        for (Index index = 0; index < target->getParams().size(); ++index) {
          if (target->getParams()[index] != Type::i32 ||
              hasPrivateParameter(target->name, index)) {
            continue;
          }
          bool allPrivate = true;
          for (const auto& site : calls->second) {
            if (index >= site.call->operands.size()) {
              allPrivate = false;
              break;
            }
            Rewriter analyzer(*this, module, *site.caller);
            if (!analyzer.expressionIsPrivate(site.call->operands[index])) {
              allPrivate = false;
              break;
            }
          }
          if (allPrivate) {
            privateParameters.insert(parameterKey(target->name, index));
            changed = true;
          }
        }
      }
    } while (changed);
  }

  void addFunctionEntryProfiling(const std::vector<Function*>& originals) {
    if (!options.profileFunctionEntries) {
      return;
    }
    Name hook("__pglite_profile_function_entry");
    if (module.getFunctionOrNull(hook)) {
      throw std::runtime_error("reserved profile hook name already exists");
    }
    auto import =
      Builder::makeFunction(hook, Signature(Type::i32, Type::none), {});
    import->module = Name("pglite");
    import->base = Name("profile_function_entry");
    module.addFunction(std::move(import));

    Builder builder(module);
    for (auto* function : originals) {
      const auto& stat = functionStats.at(function->name.toString());
      uint64_t total = 0;
      for (const auto& [_, count] : stat.operations) {
        total += count;
      }
      if (!total) {
        continue;
      }
      auto* hit = builder.makeCall(
        hook,
        {builder.makeConst(int32_t(stat.wasmFunctionIndex))},
        Type::none);
      function->body =
        builder.makeBlock({hit, function->body}, function->body->type);
    }
  }

  void run() {
    rejectAlreadyTransformed();
    initializeFunctionStats();
    initializeProvenanceSummaries();
    inferPrivateParameters();
    addGlobalMemory();

    std::vector<Function*> originals;
    for (const auto& function : module.functions) {
      if (!function->imported()) {
        originals.push_back(function.get());
      }
    }
    for (auto* function : originals) {
      currentFunction = function->name;
      Rewriter rewriter(*this, module, *function);
      rewriter.walkFunctionInModule(function, &module);
    }
    addFunctionEntryProfiling(originals);
    addHelpers();
    audit();
    replaceSourceMapURL();
    addManifest();

    if (!WasmValidator().validate(module)) {
      throw std::runtime_error("Binaryen validation failed after transformation");
    }
  }

  std::string reportJSON() const {
    auto writeMap = [](std::ostringstream& out,
                       const std::map<std::string, uint64_t>& values) {
      bool first = true;
      out << '{';
      for (const auto& [name, value] : values) {
        if (!first) {
          out << ',';
        }
        first = false;
        out << '"' << jsonEscape(name) << "\":" << value;
      }
      out << '}';
    };

    std::ostringstream out;
    out << '{' << "\"abi\":" << manifestJSON() << ",\"rewritten\":";
    writeMap(out, rewritten);
    out << ",\"directPrivate\":";
    writeMap(out, directPrivate);
    out << ",\"directPrivateProofs\":";
    writeMap(out, directPrivateProofs);
    out << ",\"privateReturnExports\":[";
    for (size_t i = 0; i < options.privateReturnExports.size(); ++i) {
      if (i) {
        out << ',';
      }
      out << '"' << jsonEscape(options.privateReturnExports[i]) << '"';
    }
    out << ']';
    out << ",\"inferredPrivateParameters\":" << privateParameters.size();
    out << ",\"allowlisted\":";
    writeMap(out, allowlisted);
    out << ",\"functions\":[";
    bool firstFunction = true;
    for (const auto& [name, stat] : functionStats) {
      if (stat.operations.empty()) {
        continue;
      }
      if (!firstFunction) {
        out << ',';
      }
      firstFunction = false;
      uint64_t total = 0;
      for (const auto& [_, count] : stat.operations) {
        total += count;
      }
      out << "{\"name\":\"" << jsonEscape(name)
          << "\",\"wasmFunctionIndex\":" << stat.wasmFunctionIndex
          << ",\"accessClassification\":\""
          << (options.privateOnlyOracle ||
                  std::find(options.directPrivateFunctionIndices.begin(),
                            options.directPrivateFunctionIndices.end(),
                            stat.wasmFunctionIndex) !=
                    options.directPrivateFunctionIndices.end()
                ? "diagnostic-direct-private"
                : options.provenance ? "mixed-provenance"
                : "generic")
          << "\""
          << ",\"expressionCount\":" << stat.expressionCount
          << ",\"expressionShapeHash\":\"" << std::hex
          << stat.expressionShapeHash << std::dec << "\""
          << ",\"staticMemoryOperations\":" << total
          << ",\"operations\":";
      writeMap(out, stat.operations);
      out << ",\"directPrivateOperations\":";
      writeMap(out, stat.directPrivateOperations);
      out << ",\"genericOperations\":";
      writeMap(out, stat.genericOperations);
      out << '}';
    }
    out << ']';
    out << ",\"helpers\":[";
    for (size_t i = 0; i < helperSpecs.size(); ++i) {
      if (i) {
        out << ',';
      }
      out << "{\"name\":\"" << jsonEscape(helperSpecs[i].first.toString())
          << "\",\"kind\":\"" << kindName(helperSpecs[i].second.kind)
          << "\"}";
    }
    out << "]}";
    return out.str();
  }
};

void Rewriter::requirePrivate(Name memory, const char* operation) {
  if (memory != transformer.getPrivateMemory()) {
    throw std::runtime_error(std::string(operation) +
                             " unexpectedly targets a non-private input memory");
  }
}

Index Rewriter::memoryNestingDepth() {
  Index depth = 0;
  for (Index i = 0; i + 1 < expressionStack.size(); ++i) {
    switch (expressionStack[i]->_id) {
      case Expression::LoadId:
      case Expression::StoreId:
      case Expression::AtomicRMWId:
      case Expression::AtomicCmpxchgId:
      case Expression::AtomicWaitId:
      case Expression::AtomicNotifyId:
      case Expression::SIMDLoadId:
      case Expression::SIMDLoadStoreLaneId:
        ++depth;
        break;
      default:
        break;
    }
  }
  return depth;
}

Index Rewriter::getOperandTemp(Index depth, Index position, Type type) {
  std::ostringstream key;
  key << depth << ':' << position << ':' << type;
  auto found = operandTemps.find(key.str());
  if (found != operandTemps.end()) {
    return found->second;
  }
  Index temp = Builder::addVar(getFunction(), type);
  operandTemps.emplace(key.str(), temp);
  return temp;
}

Rewriter::Provenance Rewriter::classify(Expression* expression) {
  if (!expression || expression->type != Type::i32) {
    return Provenance::Unknown;
  }
  auto cached = provenanceCache.find(expression);
  if (cached != provenanceCache.end()) {
    return cached->second;
  }
  if (!provenanceActive.insert(expression).second) {
    return Provenance::Unknown;
  }

  auto join = [](Provenance left, Provenance right) {
    return left == right ? left : Provenance::Unknown;
  };
  Provenance result = Provenance::Unknown;
  if (auto* constant = expression->dynCast<Const>()) {
    uint32_t value = constant->value.geti32();
    if (value == 0) {
      result = Provenance::Null;
    } else if ((value & 0x80000000U) == 0) {
      result = Provenance::Private;
    } else if ((value & 0xc0000000U) == 0x80000000U) {
      result = Provenance::Global;
    } else if ((value & 0xc0000000U) == 0xc0000000U) {
      result = Provenance::Scoped;
    }
  } else if (auto* get = expression->dynCast<LocalGet>()) {
    const auto& sets = localGraph.getSets(get);
    if (!sets.empty()) {
      std::optional<Provenance> joined;
      for (auto* set : sets) {
        Provenance source = Provenance::Unknown;
        if (set) {
          source = classify(set->value);
        } else if (function.isParam(get->index) &&
                   transformer.hasPrivateParameter(function.name, get->index)) {
          source = Provenance::Private;
        } else if (!function.isParam(get->index)) {
          source = Provenance::Null;
        }
        joined = joined ? join(*joined, source) : source;
      }
      result = *joined;
    }
  } else if (auto* set = expression->dynCast<LocalSet>()) {
    result = classify(set->value);
  } else if (auto* get = expression->dynCast<GlobalGet>()) {
    auto* global = module.getGlobalOrNull(get->name);
    if (global) {
      if (global->imported() &&
          ((global->module == Name("env") &&
            (global->base == Name("__stack_pointer") ||
             global->base == Name("__memory_base"))) ||
           global->module == Name("GOT.mem"))) {
        result = Provenance::Private;
      } else if (!global->imported() && !global->mutable_ && global->init) {
        result = classify(global->init);
      }
    }
  } else if (auto* call = expression->dynCast<Call>()) {
    if (!call->isReturn && transformer.hasPrivateReturn(call->target)) {
      result = Provenance::Private;
    }
  } else if (auto* binary = expression->dynCast<Binary>()) {
    auto left = classify(binary->left);
    auto right = classify(binary->right);
    bool leftConstant = binary->left->is<Const>();
    bool rightConstant = binary->right->is<Const>();
    if (binary->op == AddInt32) {
      if (left == Provenance::Private && rightConstant) {
        result = Provenance::Private;
      } else if (right == Provenance::Private && leftConstant) {
        result = Provenance::Private;
      }
    } else if (binary->op == SubInt32 && left == Provenance::Private &&
               rightConstant) {
      result = Provenance::Private;
    }
  } else if (auto* select = expression->dynCast<Select>()) {
    result = join(classify(select->ifTrue), classify(select->ifFalse));
  } else if (auto* block = expression->dynCast<Block>()) {
    if (!block->list.empty()) {
      result = classify(block->list.back());
    }
  } else if (auto* iff = expression->dynCast<If>()) {
    if (iff->ifFalse) {
      result = join(classify(iff->ifTrue), classify(iff->ifFalse));
    }
  }

  provenanceActive.erase(expression);
  provenanceCache.emplace(expression, result);
  return result;
}

bool Rewriter::provesPrivate(const HelperSpec& spec,
                             const std::vector<Expression*>& operands) {
  if (!transformer.useProvenance()) {
    return false;
  }
  if (classify(operands[0]) != Provenance::Private) {
    return false;
  }
  return spec.kind != HelperKind::MemoryCopy ||
         classify(operands[1]) == Provenance::Private;
}

void Rewriter::replaceWithHelper(Expression* original,
                                 const HelperSpec& spec,
                                 std::vector<Expression*> operands,
                                 Type result,
                                 const char* operationName) {
  bool diagnosticDirect = transformer.usePrivateOnlyOracle() ||
                          transformer.useCurrentFunctionDirectPrivate();
  bool provenDirect = provesPrivate(spec, operands);
  transformer.countAccess(operationName,
                          diagnosticDirect || provenDirect,
                          provenDirect ? "constant-local-flow" : nullptr);
  if (diagnosticDirect || provenDirect) {
    transformer.keepPrivateOracleOperation(original);
    return;
  }
  Builder builder(*getModule());
  auto helper = transformer.helperFor(spec);
  if (!transformer.useInlinePrivateFastPath() ||
      spec.kind == HelperKind::MemoryCopy ||
      spec.kind == HelperKind::MemoryFill) {
    replaceCurrent(builder.makeCall(helper, operands, result));
    return;
  }

  Index depth = memoryNestingDepth();
  auto params = transformer.getHelperParams(spec);
  std::vector<Expression*> prefix;
  std::vector<Expression*> privateOperands;
  std::vector<Expression*> sharedOperands;
  Index pointerTemp = 0;
  Expression* pointerForCondition = nullptr;
  for (Index i = 0; i < operands.size(); ++i) {
    Index temp = getOperandTemp(depth, i, params[i]);
    if (i == 0) {
      pointerTemp = temp;
    }
    if (i == 0 && operands.size() == 1) {
      // Loads dominate the real artifact. A tee evaluates their address once
      // while avoiding the set/get pair otherwise needed before dispatch.
      pointerForCondition =
        builder.makeLocalTee(temp, operands[i], params[i]);
    } else {
      prefix.push_back(builder.makeLocalSet(temp, operands[i]));
    }
    privateOperands.push_back(builder.makeLocalGet(temp, params[i]));
    sharedOperands.push_back(builder.makeLocalGet(temp, params[i]));
  }
  if (!pointerForCondition) {
    pointerForCondition = builder.makeLocalGet(pointerTemp, Type::i32);
  }
  auto* sharedCall = builder.makeCall(helper, sharedOperands, result);
  auto* privateOperation = transformer.makeInlinePrivateOperation(
    spec, builder, std::move(privateOperands));
  // A signed-positive test recognizes exactly the valid private-pointer
  // interval [1, 0x7fffffff]. Zero and both tagged domains are non-positive
  // and take the generic helper, retaining canonical null/tag/aperture traps.
  auto* privateCondition = builder.makeBinary(
    GtSInt32,
    pointerForCondition,
    builder.makeConst(int32_t(0)));
  auto* dispatch = builder.makeIf(
    privateCondition,
    privateOperation,
    sharedCall,
    result);
  prefix.push_back(dispatch);
  replaceCurrent(builder.makeBlock(prefix, result));
}

void Rewriter::visitLoad(Load* curr) {
  requirePrivate(curr->memory, "load");
  HelperSpec spec;
  spec.kind = HelperKind::Load;
  spec.bytes = curr->bytes;
  spec.signed_ = curr->signed_;
  spec.atomic = curr->isAtomic;
  spec.offset = curr->offset.addr;
  spec.align = curr->align.addr;
  spec.type = curr->type;
  replaceWithHelper(curr,
                    spec,
                    {curr->ptr},
                    curr->type,
                    curr->isAtomic ? "atomic-load" : "load");
}

void Rewriter::visitStore(Store* curr) {
  requirePrivate(curr->memory, "store");
  HelperSpec spec;
  spec.kind = HelperKind::Store;
  spec.bytes = curr->bytes;
  spec.atomic = curr->isAtomic;
  spec.offset = curr->offset.addr;
  spec.align = curr->align.addr;
  spec.type = Type::none;
  spec.valueType = curr->valueType;
  replaceWithHelper(curr,
                    spec,
                    {curr->ptr, curr->value},
                    Type::none,
                    curr->isAtomic ? "atomic-store" : "store");
}

void Rewriter::visitAtomicRMW(AtomicRMW* curr) {
  requirePrivate(curr->memory, "atomic.rmw");
  HelperSpec spec;
  spec.kind = HelperKind::AtomicRMW;
  spec.bytes = curr->bytes;
  spec.offset = curr->offset.addr;
  spec.align = curr->bytes;
  spec.type = curr->type;
  spec.rmwOp = curr->op;
  replaceWithHelper(
    curr, spec, {curr->ptr, curr->value}, curr->type, "atomic-rmw");
}

void Rewriter::visitAtomicCmpxchg(AtomicCmpxchg* curr) {
  requirePrivate(curr->memory, "atomic.cmpxchg");
  HelperSpec spec;
  spec.kind = HelperKind::AtomicCmpxchg;
  spec.bytes = curr->bytes;
  spec.offset = curr->offset.addr;
  spec.align = curr->bytes;
  spec.type = curr->type;
  replaceWithHelper(
    curr,
    spec,
    {curr->ptr, curr->expected, curr->replacement},
    curr->type,
    "atomic-cmpxchg");
}

void Rewriter::visitAtomicWait(AtomicWait* curr) {
  requirePrivate(curr->memory, "memory.atomic.wait");
  HelperSpec spec;
  spec.kind = HelperKind::AtomicWait;
  spec.bytes = curr->expectedType == Type::i64 ? 8 : 4;
  spec.offset = curr->offset.addr;
  spec.align = spec.bytes;
  spec.type = Type::i32;
  spec.valueType = curr->expectedType;
  replaceWithHelper(
    curr,
    spec,
    {curr->ptr, curr->expected, curr->timeout},
    Type::i32,
    "atomic-wait");
}

void Rewriter::visitAtomicNotify(AtomicNotify* curr) {
  requirePrivate(curr->memory, "memory.atomic.notify");
  HelperSpec spec;
  spec.kind = HelperKind::AtomicNotify;
  spec.bytes = 4;
  spec.offset = curr->offset.addr;
  spec.align = 4;
  spec.type = Type::i32;
  replaceWithHelper(curr,
                    spec,
                    {curr->ptr, curr->notifyCount},
                    Type::i32,
                    "atomic-notify");
}

void Rewriter::visitAtomicFence(AtomicFence*) {
  transformer.countAllowlist("atomic-fence");
}

void Rewriter::visitSIMDLoad(SIMDLoad* curr) {
  requirePrivate(curr->memory, "SIMD load");
  HelperSpec spec;
  spec.kind = HelperKind::SIMDLoad;
  spec.bytes = curr->getMemBytes();
  spec.offset = curr->offset.addr;
  spec.align = curr->align.addr;
  spec.type = Type::v128;
  spec.simdLoadOp = curr->op;
  replaceWithHelper(curr, spec, {curr->ptr}, Type::v128, "simd-load");
}

void Rewriter::visitSIMDLoadStoreLane(SIMDLoadStoreLane* curr) {
  requirePrivate(curr->memory, "SIMD lane load/store");
  HelperSpec spec;
  spec.kind = HelperKind::SIMDLoadStoreLane;
  spec.bytes = curr->getMemBytes();
  spec.offset = curr->offset.addr;
  spec.align = curr->align.addr;
  spec.type = curr->type;
  spec.simdLaneOp = curr->op;
  spec.lane = curr->index;
  spec.laneStore = curr->isStore();
  replaceWithHelper(curr,
                    spec,
                    {curr->ptr, curr->vec},
                    curr->type,
                    spec.laneStore ? "simd-lane-store" : "simd-lane-load");
}

void Rewriter::visitMemoryCopy(MemoryCopy* curr) {
  requirePrivate(curr->destMemory, "memory.copy destination");
  requirePrivate(curr->sourceMemory, "memory.copy source");
  HelperSpec spec;
  spec.kind = HelperKind::MemoryCopy;
  spec.type = Type::none;
  replaceWithHelper(curr,
                    spec,
                    {curr->dest, curr->source, curr->size},
                    Type::none,
                    "memory-copy");
}

void Rewriter::visitMemoryFill(MemoryFill* curr) {
  requirePrivate(curr->memory, "memory.fill");
  HelperSpec spec;
  spec.kind = HelperKind::MemoryFill;
  spec.type = Type::none;
  replaceWithHelper(curr,
                    spec,
                    {curr->dest, curr->value, curr->size},
                    Type::none,
                    "memory-fill");
}

void Rewriter::visitMemoryInit(MemoryInit* curr) {
  requirePrivate(curr->memory, "memory.init");
  transformer.countAllowlist("memory-init-private");
}

void Rewriter::visitMemorySize(MemorySize* curr) {
  requirePrivate(curr->memory, "memory.size");
  transformer.countAllowlist("memory-size-private");
}

void Rewriter::visitMemoryGrow(MemoryGrow* curr) {
  requirePrivate(curr->memory, "memory.grow");
  transformer.countAllowlist("memory-grow-private");
}

void Rewriter::visitDataDrop(DataDrop*) {
  transformer.countAllowlist("data-drop");
}

void enableInputFeatures(Module& module, const Options& options) {
  for (const auto& requested : options.inputFeatures) {
    bool found = false;
    for (uint32_t bit = 1; bit <= FeatureSet::CallIndirectOverlong; bit <<= 1) {
      auto feature = FeatureSet::Feature(bit);
      if (FeatureSet::toString(feature) == requested) {
        module.features.set(feature);
        found = true;
        break;
      }
    }
    if (!found) {
      throw std::runtime_error("unknown input feature: " + requested);
    }
  }
  if (!options.inputFeatures.empty()) {
    module.hasFeaturesSection = true;
  }
}

} // namespace

int main(int argc, const char** argv) {
  try {
    Options options = parseOptions(argc, argv);
    Module module;
    ModuleReader reader;
    reader.setDWARF(false);
    reader.read(options.input, module, options.inputSourceMap);
    enableInputFeatures(module, options);
    if (!WasmValidator().validate(module)) {
      throw std::runtime_error("Binaryen validation failed for input module");
    }

    Transformer transformer(module, options);
    transformer.run();

    PassOptions passOptions;
    passOptions.debugInfo = true;
    ModuleWriter writer(passOptions);
    writer.setBinary(!options.emitText);
    writer.setDebugInfo(true);
    if (!options.outputSourceMap.empty()) {
      writer.setSourceMapFilename(options.outputSourceMap);
    }
    if (!options.outputSourceMapURL.empty()) {
      writer.setSourceMapUrl(options.outputSourceMapURL);
    }
    writer.write(module, options.output);

    if (!options.report.empty()) {
      std::ofstream report(options.report);
      if (!report) {
        throw std::runtime_error("cannot open report file: " + options.report);
      }
      report << transformer.reportJSON() << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "pglite-wasm-multi-memory: " << error.what() << '\n';
    return 1;
  }
}
