// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Create a standard pipeline of passes. This pipeline should
// be used in the main driver as well as in testing.

#include "xls/passes/optimization_pass_pipeline.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/check.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/common/logging/logging.h"
#include "xls/common/module_initializer.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/package.h"
#include "xls/passes/arith_simplification_pass.h"
#include "xls/passes/array_simplification_pass.h"
#include "xls/passes/bdd_cse_pass.h"
#include "xls/passes/bdd_simplification_pass.h"
#include "xls/passes/bit_slice_simplification_pass.h"
#include "xls/passes/boolean_simplification_pass.h"
#include "xls/passes/canonicalization_pass.h"
#include "xls/passes/channel_legalization_pass.h"
#include "xls/passes/comparison_simplification_pass.h"
#include "xls/passes/concat_simplification_pass.h"
#include "xls/passes/conditional_specialization_pass.h"
#include "xls/passes/constant_folding_pass.h"
#include "xls/passes/cse_pass.h"
#include "xls/passes/dataflow_simplification_pass.h"
#include "xls/passes/dce_pass.h"
#include "xls/passes/dfe_pass.h"
#include "xls/passes/identity_removal_pass.h"
#include "xls/passes/inlining_pass.h"
#include "xls/passes/label_recovery_pass.h"
#include "xls/passes/map_inlining_pass.h"
#include "xls/passes/narrowing_pass.h"
#include "xls/passes/next_value_optimization_pass.h"
#include "xls/passes/optimization_pass.h"
#include "xls/passes/optimization_pass_registry.h"
#include "xls/passes/pass_base.h"
#include "xls/passes/proc_inlining_pass.h"
#include "xls/passes/proc_state_flattening_pass.h"
#include "xls/passes/proc_state_optimization_pass.h"
#include "xls/passes/ram_rewrite_pass.h"
#include "xls/passes/reassociation_pass.h"
#include "xls/passes/receive_default_value_simplification_pass.h"
#include "xls/passes/select_simplification_pass.h"
#include "xls/passes/sparsify_select_pass.h"
#include "xls/passes/strength_reduction_pass.h"
#include "xls/passes/table_switch_pass.h"
#include "xls/passes/token_dependency_pass.h"
#include "xls/passes/token_simplification_pass.h"
#include "xls/passes/unroll_pass.h"
#include "xls/passes/useless_assert_removal_pass.h"
#include "xls/passes/useless_io_removal_pass.h"
#include "xls/passes/verifier_checker.h"

namespace xls {

namespace {

void AddSimplificationPasses(OptimizationCompoundPass& pass,
                             int64_t opt_level) {
  pass.Add<IdentityRemovalPass>();
  pass.Add<ConstantFoldingPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<CanonicalizationPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ArithSimplificationPass>(opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ComparisonSimplificationPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<TableSwitchPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ReceiveDefaultValueSimplificationPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<SelectSimplificationPass>(opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ConditionalSpecializationPass>(/*use_bdd=*/false);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ReassociationPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ConstantFoldingPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<BitSliceSimplificationPass>(opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ConcatSimplificationPass>(opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<DataflowSimplificationPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<StrengthReductionPass>(opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ArraySimplificationPass>(opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<CsePass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<ArithSimplificationPass>(opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<NarrowingPass>(/*analysis=*/NarrowingPass::AnalysisType::kTernary,
                          opt_level);
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<BooleanSimplificationPass>();
  pass.Add<DeadCodeEliminationPass>();
  pass.Add<TokenSimplificationPass>();
  pass.Add<DeadCodeEliminationPass>();
}

}  // namespace

SimplificationPass::SimplificationPass(int64_t opt_level)
    : OptimizationCompoundPass("simp", "Simplification") {
  AddSimplificationPasses(*this, opt_level);
}

FixedPointSimplificationPass::FixedPointSimplificationPass(int64_t opt_level)
    : OptimizationFixedPointCompoundPass("fixedpoint_simp",
                                         "Fixed-point Simplification") {
  AddSimplificationPasses(*this, opt_level);
}

std::unique_ptr<OptimizationCompoundPass> CreateOptimizationPassPipeline(
    int64_t opt_level) {
  auto top = std::make_unique<OptimizationCompoundPass>(
      "ir", "Top level pass pipeline");
  top->AddInvariantChecker<VerifierChecker>();

  top->Add<DeadFunctionEliminationPass>();
  top->Add<DeadCodeEliminationPass>();
  // At this stage in the pipeline only optimizations up to level 2 should
  // run. 'opt_level' is the maximum level of optimization which should be run
  // in the entire pipeline so set the level of the simplification pass to the
  // minimum of the two values. Same below.
  top->Add<SimplificationPass>(std::min(int64_t{2}, opt_level));
  top->Add<UnrollPass>();
  top->Add<MapInliningPass>();
  top->Add<InliningPass>();
  top->Add<DeadFunctionEliminationPass>();

  top->Add<FixedPointSimplificationPass>(std::min(int64_t{2}, opt_level));

  top->Add<BddSimplificationPass>(std::min(int64_t{2}, opt_level));
  top->Add<DeadCodeEliminationPass>();
  top->Add<BddCsePass>();
  // TODO(https://github.com/google/xls/issues/274): 2022/01/20 Remove this
  // extra conditional specialization pass when the pipeline has been
  // reorganized better follow a high level of abstraction down to low level.
  top->Add<DeadCodeEliminationPass>();
  top->Add<ConditionalSpecializationPass>(/*use_bdd=*/true);

  top->Add<DeadCodeEliminationPass>();
  top->Add<FixedPointSimplificationPass>(std::min(int64_t{2}, opt_level));

  top->Add<NarrowingPass>(
      /*analysis=*/NarrowingPass::AnalysisType::kRangeWithOptionalContext,
      opt_level);
  top->Add<DeadCodeEliminationPass>();
  top->Add<ArithSimplificationPass>(opt_level);
  top->Add<DeadCodeEliminationPass>();
  top->Add<CsePass>();
  top->Add<SparsifySelectPass>();
  top->Add<DeadCodeEliminationPass>();
  top->Add<UselessAssertRemovalPass>();
  top->Add<RamRewritePass>();
  top->Add<UselessIORemovalPass>();
  top->Add<DeadCodeEliminationPass>();

  // Run ConditionalSpecializationPass before TokenDependencyPass to remove
  // false data dependencies
  top->Add<ConditionalSpecializationPass>(/*use_bdd=*/true);
  // Legalize multiple channel operations before proc inlining. The legalization
  // can add an adapter proc that should be inlined.
  top->Add<ChannelLegalizationPass>();
  top->Add<TokenDependencyPass>();
  // Simplify the adapter procs before inlining.
  top->Add<FixedPointSimplificationPass>(std::min(int64_t{2}, opt_level));
  top->Add<ProcInliningPass>();

  // After proc inlining flatten and optimize the proc state. Run tuple
  // simplification to simplify tuple structures left over from flattening.
  // TODO(meheff): Consider running proc state optimization more than once.
  top->Add<ProcStateFlatteningPass>();
  top->Add<IdentityRemovalPass>();
  top->Add<DataflowSimplificationPass>();
  top->Add<NextValueOptimizationPass>(std::min(int64_t{3}, opt_level));
  top->Add<ProcStateOptimizationPass>();
  top->Add<DeadCodeEliminationPass>();

  top->Add<BddSimplificationPass>(std::min(int64_t{3}, opt_level));
  top->Add<DeadCodeEliminationPass>();
  top->Add<BddCsePass>();
  top->Add<DeadCodeEliminationPass>();

  top->Add<ConditionalSpecializationPass>(/*use_bdd=*/true);
  top->Add<DeadCodeEliminationPass>();

  top->Add<FixedPointSimplificationPass>(std::min(int64_t{3}, opt_level));

  top->Add<UselessAssertRemovalPass>();
  top->Add<UselessIORemovalPass>();
  top->Add<NextValueOptimizationPass>(std::min(int64_t{3}, opt_level));
  top->Add<ProcStateOptimizationPass>();
  top->Add<DeadCodeEliminationPass>();

  top->Add<LabelRecoveryPass>();
  return top;
}

absl::StatusOr<bool> RunOptimizationPassPipeline(Package* package,
                                                 int64_t opt_level) {
  std::unique_ptr<OptimizationCompoundPass> pipeline =
      CreateOptimizationPassPipeline();
  PassResults results;
  return pipeline->Run(package, OptimizationPassOptions(), &results);
}

absl::Status OptimizationPassPipelineGenerator::AddPassToPipeline(
    OptimizationCompoundPass* pipeline, std::string_view pass_name) const {
  XLS_ASSIGN_OR_RETURN(auto* generator,
                       GetOptimizationRegistry().Generator(pass_name));
  return generator->AddToPipeline(pipeline, opt_level_);
}
std::string OptimizationPassPipelineGenerator::GetAvailablePassesStr() const {
  std::ostringstream oss;
  oss << "[";
  bool first = true;
  auto all_passes = GetAvailablePasses();
  absl::c_sort(all_passes);
  for (auto v : all_passes) {
    if (!first) {
      oss << ", ";
    }
    first = false;
    oss << v;
  }
  oss << "]";
  return oss.str();
}

std::vector<std::string_view>
OptimizationPassPipelineGenerator::GetAvailablePasses() const {
  return GetOptimizationRegistry().GetRegisteredNames();
}

XLS_REGISTER_MODULE_INITIALIZER(simp_pass, {
  CHECK_OK(RegisterOptimizationPass<FixedPointSimplificationPass>(
      "fixedpoint_simp", pass_config::kOptLevel));
  CHECK_OK(RegisterOptimizationPass<FixedPointSimplificationPass>(
      "fixedpoint_simp(2)", pass_config::CappedOptLevel{2}));
  CHECK_OK(RegisterOptimizationPass<FixedPointSimplificationPass>(
      "fixedpoint_simp(3)", pass_config::CappedOptLevel{3}));
  CHECK_OK(RegisterOptimizationPass<SimplificationPass>(
      "simp", pass_config::kOptLevel));
  CHECK_OK(RegisterOptimizationPass<SimplificationPass>(
      "simp(2)", pass_config::CappedOptLevel{2}));
  CHECK_OK(RegisterOptimizationPass<SimplificationPass>(
      "simp(3)", pass_config::CappedOptLevel{3}));
});

}  // namespace xls
