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

#include "xls/dslx/type_system/deduce.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/cleanup/cleanup.h"
#include "absl/container/btree_set.h"
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "absl/types/variant.h"
#include "xls/common/casts.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/common/visitor.h"
#include "xls/dslx/bytecode/bytecode.h"
#include "xls/dslx/bytecode/bytecode_emitter.h"
#include "xls/dslx/bytecode/bytecode_interpreter.h"
#include "xls/dslx/channel_direction.h"
#include "xls/dslx/constexpr_evaluator.h"
#include "xls/dslx/errors.h"
#include "xls/dslx/frontend/ast.h"
#include "xls/dslx/frontend/ast_node.h"
#include "xls/dslx/frontend/ast_utils.h"
#include "xls/dslx/frontend/module.h"
#include "xls/dslx/frontend/pos.h"
#include "xls/dslx/frontend/token_utils.h"
#include "xls/dslx/import_data.h"
#include "xls/dslx/interp_bindings.h"
#include "xls/dslx/interp_value.h"
#include "xls/dslx/type_system/deduce_ctx.h"
#include "xls/dslx/type_system/deduce_enum_def.h"
#include "xls/dslx/type_system/deduce_expr.h"
#include "xls/dslx/type_system/deduce_invocation.h"
#include "xls/dslx/type_system/deduce_struct_def.h"
#include "xls/dslx/type_system/deduce_utils.h"
#include "xls/dslx/type_system/parametric_constraint.h"
#include "xls/dslx/type_system/parametric_env.h"
#include "xls/dslx/type_system/parametric_expression.h"
#include "xls/dslx/type_system/parametric_instantiator.h"
#include "xls/dslx/type_system/type.h"
#include "xls/dslx/type_system/type_and_parametric_env.h"
#include "xls/dslx/type_system/type_info.h"
#include "xls/dslx/type_system/unwrap_meta_type.h"
#include "xls/dslx/warning_kind.h"
#include "xls/ir/bits.h"

namespace xls::dslx {
namespace {

// Attempts to convert an expression from the full DSL AST into the
// ParametricExpression sub-AST (a limited form that we can embed into a
// TypeDim for later instantiation).
absl::StatusOr<std::unique_ptr<ParametricExpression>> ExprToParametric(
    const Expr* e, DeduceCtx* ctx) {
  if (auto* n = dynamic_cast<const ConstRef*>(e)) {
    XLS_RETURN_IF_ERROR(ctx->Deduce(n).status());
    XLS_ASSIGN_OR_RETURN(InterpValue constant,
                         ctx->type_info()->GetConstExpr(n));
    return std::make_unique<ParametricConstant>(std::move(constant));
  }
  if (auto* n = dynamic_cast<const NameRef*>(e)) {
    return std::make_unique<ParametricSymbol>(n->identifier(), n->span());
  }
  if (auto* n = dynamic_cast<const Binop*>(e)) {
    XLS_ASSIGN_OR_RETURN(auto lhs, ExprToParametric(n->lhs(), ctx));
    XLS_ASSIGN_OR_RETURN(auto rhs, ExprToParametric(n->rhs(), ctx));
    switch (n->binop_kind()) {
      case BinopKind::kMul:
        return std::make_unique<ParametricMul>(std::move(lhs), std::move(rhs));
      case BinopKind::kAdd:
        return std::make_unique<ParametricAdd>(std::move(lhs), std::move(rhs));
      default:
        return absl::InvalidArgumentError(
            "Cannot convert expression to parametric: " + e->ToString());
    }
  }
  if (auto* n = dynamic_cast<const Number*>(e)) {
    auto default_type = BitsType::MakeU32();
    XLS_ASSIGN_OR_RETURN(
        InterpValue constexpr_value,
        ConstexprEvaluator::EvaluateToValue(
            ctx->import_data(), ctx->type_info(), ctx->warnings(),
            ctx->GetCurrentParametricEnv(), n, default_type.get()));
    return std::make_unique<ParametricConstant>(std::move(constexpr_value));
  }
  return absl::InvalidArgumentError(
      "Cannot convert expression to parametric: " + e->ToString());
}

absl::StatusOr<InterpValue> InterpretExpr(
    DeduceCtx* ctx, const Expr* expr,
    const absl::flat_hash_map<std::string, InterpValue>& env) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<BytecodeFunction> bf,
                       BytecodeEmitter::EmitExpression(
                           ctx->import_data(), ctx->type_info(), expr, env,
                           ctx->fn_stack().back().parametric_env()));

  return BytecodeInterpreter::Interpret(ctx->import_data(), bf.get(),
                                        /*args=*/{});
}

// Resolves "ref" to an AST proc.
absl::StatusOr<Proc*> ResolveColonRefToProc(const ColonRef* ref,
                                            DeduceCtx* ctx) {
  std::optional<Import*> import = ref->ResolveImportSubject();
  XLS_RET_CHECK(import.has_value())
      << "ColonRef did not refer to an import: " << ref->ToString();
  std::optional<const ImportedInfo*> imported_info =
      ctx->type_info()->GetImported(*import);
  return GetMemberOrTypeInferenceError<Proc>(imported_info.value()->module,
                                             ref->attr(), ref->span());
}

absl::StatusOr<std::unique_ptr<Type>> DeduceProcMember(const ProcMember* node,
                                                       DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(auto type, ctx->Deduce(node->type_annotation()));
  auto* meta_type = dynamic_cast<MetaType*>(type.get());
  std::unique_ptr<Type>& param_type = meta_type->wrapped();
  return std::move(param_type);
}

absl::StatusOr<std::unique_ptr<Type>> DeduceParam(const Param* node,
                                                  DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(auto type, ctx->Deduce(node->type_annotation()));
  auto* meta_type = dynamic_cast<MetaType*>(type.get());
  std::unique_ptr<Type>& param_type = meta_type->wrapped();

  Function* f = dynamic_cast<Function*>(node->parent());
  if (f == nullptr) {
    return std::move(param_type);
  }

  // Special case handling for parameters to config functions. These must be
  // made constexpr.
  //
  // When deducing a proc at top level, we won't have constexpr values for its
  // config params, which will cause Spawn deduction to fail, so we need to
  // create dummy InterpValues for its parameter channels.
  // Other types of params aren't allowed, example: a proc member could be
  // assigned a constexpr value based on the sum of dummy values.
  // Stack depth 2: Module "<top>" + the config function being looked at.
  bool is_root_proc =
      f->tag() == FunctionTag::kProcConfig && ctx->fn_stack().size() == 2;
  bool is_channel_param =
      dynamic_cast<ChannelType*>(param_type.get()) != nullptr;
  bool is_param_constexpr = ctx->type_info()->IsKnownConstExpr(node);
  if (is_root_proc && is_channel_param && !is_param_constexpr) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue value,
        ConstexprEvaluator::CreateChannelValue(param_type.get()));
    ctx->type_info()->NoteConstExpr(node, value);
    ctx->type_info()->NoteConstExpr(node->name_def(), value);
  }

  return std::move(param_type);
}

// It's common to accidentally use different constant naming conventions
// coming from other environments -- warn folks if it's not following
// https://doc.rust-lang.org/1.0.0/style/style/naming/README.html
static void WarnOnInappropriateConstantName(std::string_view identifier,
                                            const Span& span,
                                            const Module& module,
                                            DeduceCtx* ctx) {
  if (!IsScreamingSnakeCase(identifier) &&
      !module.annotations().contains(
          ModuleAnnotation::kAllowNonstandardConstantNaming)) {
    ctx->warnings()->Add(
        span, WarningKind::kConstantNaming,
        absl::StrFormat("Standard style is SCREAMING_SNAKE_CASE for constant "
                        "identifiers; got: `%s`",
                        identifier));
  }
}

absl::StatusOr<std::unique_ptr<Type>> DeduceConstantDef(const ConstantDef* node,
                                                        DeduceCtx* ctx) {
  XLS_VLOG(5) << "Noting constant: " << node->ToString();
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> result,
                       ctx->Deduce(node->value()));
  const FnStackEntry& peek_entry = ctx->fn_stack().back();
  std::optional<FnCtx> fn_ctx;
  if (peek_entry.f() != nullptr) {
    fn_ctx.emplace(FnCtx{peek_entry.module()->name(), peek_entry.name(),
                         peek_entry.parametric_env()});
  }

  ctx->type_info()->SetItem(node, *result);
  ctx->type_info()->SetItem(node->name_def(), *result);

  if (node->type_annotation() != nullptr) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> annotated,
                         ctx->Deduce(node->type_annotation()));
    XLS_ASSIGN_OR_RETURN(
        annotated,
        UnwrapMetaType(std::move(annotated), node->type_annotation()->span(),
                       "numeric literal type-prefix"));
    if (*annotated != *result) {
      return ctx->TypeMismatchError(node->span(), node->type_annotation(),
                                    *annotated, node->value(), *result,
                                    "Constant definition's annotated type did "
                                    "not match its expression's type");
    }
  }

  WarnOnInappropriateConstantName(node->identifier(), node->span(),
                                  *node->owner(), ctx);

  XLS_ASSIGN_OR_RETURN(
      InterpValue constexpr_value,
      ConstexprEvaluator::EvaluateToValue(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          ctx->GetCurrentParametricEnv(), node->value(), result.get()));
  ctx->type_info()->NoteConstExpr(node, constexpr_value);
  ctx->type_info()->NoteConstExpr(node->value(), constexpr_value);
  ctx->type_info()->NoteConstExpr(node->name_def(), constexpr_value);
  return result;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceTypeRef(const TypeRef* node,
                                                    DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type,
                       ctx->Deduce(ToAstNode(node->type_definition())));
  XLS_RET_CHECK(type->IsMeta()) << type->ToString();
  return type;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceTypeAlias(const TypeAlias* node,
                                                      DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type,
                       ctx->Deduce(node->type_annotation()));
  XLS_RET_CHECK(type->IsMeta());
  ctx->type_info()->SetItem(node->name_def(), *type);
  return type;
}

// Typechecks the name def tree items against type, putting the corresponding
// type information for the AST nodes within the name_def_tree as corresponding
// to the types within "type" (recursively).
//
// For example:
//
//    (a, (b, c))  vs (u8, (u4, u2))
//
// Will put a correspondence of {a: u8, b: u4, c: u2} into the mapping in ctx.
static absl::Status BindNames(const NameDefTree* name_def_tree,
                              const Type& type, DeduceCtx* ctx,
                              std::optional<InterpValue> constexpr_value) {
  if (name_def_tree->is_leaf()) {
    AstNode* name_def = ToAstNode(name_def_tree->leaf());

    ctx->type_info()->SetItem(name_def, type);
    if (constexpr_value.has_value()) {
      ctx->type_info()->NoteConstExpr(name_def, constexpr_value.value());
    }
    return absl::OkStatus();
  }

  auto* tuple_type = dynamic_cast<const TupleType*>(&type);
  if (tuple_type == nullptr) {
    return TypeInferenceErrorStatus(
        name_def_tree->span(), &type,
        absl::StrFormat("Expected a tuple type for these names, but "
                        "got %s.",
                        type.ToString()));
  }

  if (name_def_tree->nodes().size() != tuple_type->size()) {
    return TypeInferenceErrorStatus(
        name_def_tree->span(), &type,
        absl::StrFormat("Could not bind names, names are mismatched "
                        "in number vs type; at this level of the tuple: %d "
                        "names, %d types.",
                        name_def_tree->nodes().size(), tuple_type->size()));
  }

  for (int64_t i = 0; i < name_def_tree->nodes().size(); ++i) {
    NameDefTree* subtree = name_def_tree->nodes()[i];
    const Type& subtype = tuple_type->GetMemberType(i);
    ctx->type_info()->SetItem(subtree, subtype);

    std::optional<InterpValue> sub_value;
    if (constexpr_value.has_value()) {
      sub_value = constexpr_value.value().GetValuesOrDie()[i];
    }
    XLS_RETURN_IF_ERROR(BindNames(subtree, subtype, ctx, sub_value));
  }

  return absl::OkStatus();
}

absl::StatusOr<std::unique_ptr<Type>> DeduceLet(const Let* node,
                                                DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> rhs,
                       DeduceAndResolve(node->rhs(), ctx));

  if (node->type_annotation() != nullptr) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> annotated,
                         DeduceAndResolve(node->type_annotation(), ctx));
    XLS_ASSIGN_OR_RETURN(
        annotated,
        UnwrapMetaType(std::move(annotated), node->type_annotation()->span(),
                       "let type-annotation"));
    if (*rhs != *annotated) {
      return ctx->TypeMismatchError(
          node->type_annotation()->span(), nullptr, *annotated, nullptr, *rhs,
          "Annotated type did not match inferred type "
          "of right hand side expression.");
    }
  }

  std::optional<InterpValue> maybe_constexpr_value;
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(),
      ctx->GetCurrentParametricEnv(), node->rhs(), rhs.get()));
  if (ctx->type_info()->IsKnownConstExpr(node->rhs())) {
    XLS_ASSIGN_OR_RETURN(maybe_constexpr_value,
                         ctx->type_info()->GetConstExpr(node->rhs()));
  }

  XLS_RETURN_IF_ERROR(
      BindNames(node->name_def_tree(), *rhs, ctx, maybe_constexpr_value));

  if (node->name_def_tree()->IsWildcardLeaf()) {
    ctx->warnings()->Add(
        node->name_def_tree()->span(), WarningKind::kUselessLetBinding,
        "`let _ = expr;` statement can be simplified to `expr;` -- there is no "
        "need for a `let` binding here");
  }

  if (node->is_const()) {
    TypeInfo* ti = ctx->type_info();
    XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                         ti->GetConstExpr(node->rhs()));
    ti->NoteConstExpr(node, constexpr_value);
    // Reminder: we don't allow name destructuring in constant defs, so this
    // is expected to never fail.
    XLS_RET_CHECK_EQ(node->name_def_tree()->GetNameDefs().size(), 1);

    NameDef* name_def = node->name_def_tree()->GetNameDefs()[0];
    ti->NoteConstExpr(name_def, ti->GetConstExpr(node->rhs()).value());

    WarnOnInappropriateConstantName(name_def->identifier(), node->span(),
                                    *node->owner(), ctx);
  }

  return Type::MakeUnit();
}

absl::StatusOr<std::unique_ptr<Type>> DeduceFor(const For* node,
                                                DeduceCtx* ctx) {
  // Type of the init value to the for loop (also the accumulator type).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> init_type,
                       DeduceAndResolve(node->init(), ctx));

  // Type of the iterable (whose elements are being used as the induction
  // variable in the for loop).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> iterable_type,
                       DeduceAndResolve(node->iterable(), ctx));
  auto* iterable_array_type = dynamic_cast<ArrayType*>(iterable_type.get());
  if (iterable_array_type == nullptr) {
    return TypeInferenceErrorStatus(node->iterable()->span(),
                                    iterable_type.get(),
                                    "For loop iterable value is not an array.");
  }
  const Type& iterable_element_type = iterable_array_type->element_type();

  std::vector<std::unique_ptr<Type>> target_annotated_type_elems;
  target_annotated_type_elems.push_back(iterable_element_type.CloneToUnique());
  target_annotated_type_elems.push_back(init_type->CloneToUnique());
  auto target_annotated_type =
      std::make_unique<TupleType>(std::move(target_annotated_type_elems));

  // If there was an explicitly annotated type, ensure it matches our inferred
  // one.
  if (node->type_annotation() != nullptr) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> annotated_type,
                         DeduceAndResolve(node->type_annotation(), ctx));
    XLS_ASSIGN_OR_RETURN(annotated_type,
                         UnwrapMetaType(std::move(annotated_type),
                                        node->type_annotation()->span(),
                                        "for-loop annotated type"));

    if (*target_annotated_type != *annotated_type) {
      return ctx->TypeMismatchError(
          node->span(), node->type_annotation(), *annotated_type, nullptr,
          *target_annotated_type,
          "For-loop annotated type did not match inferred type.");
    }
  }

  // Bind the names to their associated types for use in the body.
  NameDefTree* bindings = node->names();

  if (!bindings->IsIrrefutable()) {
    return TypeInferenceErrorStatus(
        bindings->span(), nullptr,
        absl::StrFormat("for-loop bindings must be irrefutable (i.e. the "
                        "pattern must match all possible values)"));
  }

  XLS_RETURN_IF_ERROR(
      BindNames(bindings, *target_annotated_type, ctx, std::nullopt));

  // Now we can deduce the body.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> body_type,
                       DeduceAndResolve(node->body(), ctx));

  if (*init_type != *body_type) {
    return ctx->TypeMismatchError(node->span(), node->init(), *init_type,
                                  node->body(), *body_type,
                                  "For-loop init value type did not match "
                                  "for-loop body's result type.");
  }

  return init_type;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceUnrollFor(const UnrollFor* node,
                                                      DeduceCtx* ctx) {
  XLS_RETURN_IF_ERROR(ctx->Deduce(node->iterable()).status());
  XLS_RETURN_IF_ERROR(ctx->Deduce(node->types()).status());
  return ctx->Deduce(node->body());
}

// Returns true if the cast-conversion from "from" to "to" is acceptable (i.e.
// should not cause a type error to occur).
static bool IsAcceptableCast(const Type& from, const Type& to) {
  auto is_enum = [](const Type& ct) -> bool {
    return dynamic_cast<const EnumType*>(&ct) != nullptr;
  };
  auto is_bits_array = [&](const Type& ct) -> bool {
    const ArrayType* at = dynamic_cast<const ArrayType*>(&ct);
    if (at == nullptr) {
      return false;
    }
    if (IsBitsLike(at->element_type())) {
      return true;
    }
    return false;
  };
  if ((is_bits_array(from) && IsBitsLike(to)) ||
      (IsBitsLike(from) && is_bits_array(to))) {
    return from.GetTotalBitCount() == to.GetTotalBitCount();
  }
  if ((IsBitsLike(from) || is_enum(from)) && IsBitsLike(to)) {
    return true;
  }
  if (IsBitsLike(from) && is_enum(to)) {
    return true;
  }
  return false;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceCast(const Cast* node,
                                                 DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type,
                       DeduceAndResolve(node->type_annotation(), ctx));
  XLS_ASSIGN_OR_RETURN(
      type, UnwrapMetaType(std::move(type), node->type_annotation()->span(),
                           "cast type"));

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> expr,
                       DeduceAndResolve(node->expr(), ctx));

  if (!IsAcceptableCast(/*from=*/*expr, /*to=*/*type)) {
    return ctx->TypeMismatchError(
        node->span(), node->expr(), *expr, node->type_annotation(), *type,
        absl::StrFormat("Cannot cast from expression type %s to %s.",
                        expr->ToErrorString(), type->ToErrorString()));
  }
  return type;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceConstAssert(const ConstAssert* node,
                                                        DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type,
                       DeduceAndResolve(node->arg(), ctx));
  auto want = BitsType::MakeU1();
  if (*type != *want) {
    return ctx->TypeMismatchError(
        node->span(), /*lhs_node=*/node->arg(), *type, nullptr, *want,
        "const_assert! takes a (constexpr) boolean argument");
  }

  const ParametricEnv parametric_env = ctx->GetCurrentParametricEnv();
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(), parametric_env,
      node->arg(), type.get()));
  if (!ctx->type_info()->IsKnownConstExpr(node->arg())) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("const_assert! expression is not constexpr"));
  }

  XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                       ctx->type_info()->GetConstExpr(node->arg()));
  if (constexpr_value.IsFalse()) {
    XLS_ASSIGN_OR_RETURN(
        auto constexpr_map,
        MakeConstexprEnv(ctx->import_data(), ctx->type_info(), ctx->warnings(),
                         node->arg(), parametric_env));
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("const_assert! failure: `%s` constexpr environment: %s",
                        node->arg()->ToString(),
                        EnvMapToString(constexpr_map)));
  }

  return type;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceAttr(const Attr* node,
                                                 DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type, ctx->Deduce(node->lhs()));
  auto* struct_type = dynamic_cast<StructType*>(type.get());
  if (struct_type == nullptr) {
    return TypeInferenceErrorStatus(node->span(), type.get(),
                                    absl::StrFormat("Expected a struct for "
                                                    "attribute access; got %s",
                                                    type->ToString()));
  }

  std::string_view attr_name = node->attr();
  if (!struct_type->HasNamedMember(attr_name)) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Struct '%s' does not have a "
                        "member with name "
                        "'%s'",
                        struct_type->nominal_type().identifier(), attr_name));
  }

  std::optional<const Type*> result =
      struct_type->GetMemberTypeByName(attr_name);
  XLS_RET_CHECK(result.has_value());  // We checked above we had named member.

  auto result_type = result.value()->CloneToUnique();
  return result_type;
}

// Returns whether "e" is definitely a meaningless expression-statement; i.e. if
// in statement context definitely has no side-effects and thus should be
// flagged.
//
// Note that some invocations of functions will have no side-effects and will be
// meaningless, but because we don't look inside of callees to see if they are
// side-effecting, we conservatively mark those as potentially useful.
static bool DefinitelyMeaninglessExpression(Expr* e) {
  absl::StatusOr<std::vector<AstNode*>> nodes_under_e =
      CollectUnder(e, /*want_types=*/true);
  if (!nodes_under_e.ok()) {
    XLS_LOG(WARNING) << "Could not collect nodes under `" << e->ToString()
                     << "`; status: " << nodes_under_e.status();
    return false;
  }
  for (AstNode* n : nodes_under_e.value()) {
    // In the DSL side effects can only be caused by invocations or
    // invocation-like AST nodes.
    switch (n->kind()) {
      case AstNodeKind::kInvocation:
      case AstNodeKind::kFormatMacro:
      case AstNodeKind::kSpawn:
        return false;
      default:
        continue;
    }
  }
  return true;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceStatement(const Statement* node,
                                                      DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> result,
                       ctx->Deduce(ToAstNode(node->wrapped())));
  return result;
}

// Warns if the next-to-last statement in a block has a trailing semi and the
// last statement is a nil tuple expression, as this is redundant; i.e.
//
//    {
//      foo;
//      ()  <-- useless, semi on previous statement implies it
//    }
static void DetectUselessTrailingTuplePattern(const Block* block,
                                              DeduceCtx* ctx) {
  // TODO(https://github.com/google/xls/issues/1124) 2023-08-31 Proc config
  // parsing functions synthesize a tuple at the end, and we don't want to flag
  // that since the user didn't even create it.
  if (block->parent()->kind() == AstNodeKind::kFunction &&
      dynamic_cast<const Function*>(block->parent())->tag() ==
          FunctionTag::kProcConfig) {
    return;
  }

  // Need at least a statement (i.e. with semicolon after it) and an
  // expression-statement at the end to match this pattern.
  if (block->statements().size() < 2) {
    return;
  }

  // Trailing statement has to be an expression-statement.
  const Statement* last_stmt = block->statements().back();
  if (!std::holds_alternative<Expr*>(last_stmt->wrapped())) {
    return;
  }

  // It has to be a tuple.
  const auto* last_expr = std::get<Expr*>(last_stmt->wrapped());
  auto* trailing_tuple = dynamic_cast<const XlsTuple*>(last_expr);
  if (trailing_tuple == nullptr) {
    return;
  }

  // Tuple has to be nil.
  if (!trailing_tuple->empty()) {
    return;
  }

  ctx->warnings()->Add(
      trailing_tuple->span(), WarningKind::kTrailingTupleAfterSemi,
      absl::StrFormat("Block has a trailing nil (empty) tuple after a "
                      "semicolon -- this is implied, please remove it"));
}

absl::StatusOr<std::unique_ptr<Type>> DeduceBlock(const Block* node,
                                                  DeduceCtx* ctx) {
  std::unique_ptr<Type> last;
  for (const Statement* s : node->statements()) {
    XLS_ASSIGN_OR_RETURN(last, ctx->Deduce(s));
  }
  // If there's a trailing semicolon this block always yields unit `()`.
  if (node->trailing_semi()) {
    last = Type::MakeUnit();
  }

  // We only want to check the last statement for "useless expression-statement"
  // property if it is not yielding a value from a block; e.g.
  //
  //    {
  //      my_invocation!();
  //      u32:42  // <- ok, no trailing semi
  //    }
  //
  // vs
  //
  //    {
  //      my_invocation!();
  //      u32:42;  // <- useless, trailing semi means block yields nil
  //    }
  const bool should_check_last_statement = node->trailing_semi();
  for (int64_t i = 0; i < static_cast<int64_t>(node->statements().size()) -
                              (should_check_last_statement ? 0 : 1);
       ++i) {
    const Statement* s = node->statements()[i];
    if (std::holds_alternative<Expr*>(s->wrapped()) &&
        DefinitelyMeaninglessExpression(std::get<Expr*>(s->wrapped()))) {
      Expr* e = std::get<Expr*>(s->wrapped());
      ctx->warnings()->Add(e->span(), WarningKind::kUselessExpressionStatement,
                           absl::StrFormat("Expression statement `%s` appears "
                                           "useless (i.e. has no side-effects)",
                                           e->ToString()));
    }
  }

  DetectUselessTrailingTuplePattern(node, ctx);
  return last;
}

// Deduces a colon-ref in the particular case when the subject is known to be an
// import.
static absl::StatusOr<std::unique_ptr<Type>> DeduceColonRefToModule(
    const ColonRef* node, Module* module, DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceColonRefToModule; node: `" << node->ToString() << "`";
  std::optional<ModuleMember*> elem = module->FindMemberWithName(node->attr());
  if (!elem.has_value()) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Attempted to refer to module %s member '%s' "
                        "which does not exist.",
                        module->name(), node->attr()));
  }
  if (!IsPublic(*elem.value())) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Attempted to refer to module member %s that "
                        "is not public.",
                        ToAstNode(*elem.value())->ToString()));
  }

  XLS_ASSIGN_OR_RETURN(TypeInfo * imported_type_info,
                       ctx->import_data()->GetRootTypeInfo(module));
  if (std::holds_alternative<Function*>(*elem.value())) {
    auto* f_ptr = std::get<Function*>(*elem.value());
    XLS_RET_CHECK(f_ptr != nullptr);
    auto& f = *f_ptr;

    if (!imported_type_info->Contains(f.name_def())) {
      XLS_VLOG(2) << "Function name not in imported_type_info; indicates it is "
                     "parametric.";
      XLS_RET_CHECK(f.IsParametric());
      // We don't type check parametric functions until invocations.
      // Let's typecheck this imported parametric function with respect to its
      // module (this will only get the type signature, the body gets
      // typechecked after parametric instantiation).
      std::unique_ptr<DeduceCtx> imported_ctx =
          ctx->MakeCtx(imported_type_info, module);
      const FnStackEntry& peek_entry = ctx->fn_stack().back();
      imported_ctx->AddFnStackEntry(peek_entry);
      XLS_RETURN_IF_ERROR(ctx->typecheck_function()(f, imported_ctx.get()));
      imported_type_info = imported_ctx->type_info();
    }
  }

  AstNode* member_node = ToAstNode(*elem.value());
  std::optional<Type*> type = imported_type_info->GetItem(member_node);
  XLS_RET_CHECK(type.has_value()) << member_node->ToString();
  return type.value()->CloneToUnique();
}

static absl::StatusOr<std::unique_ptr<Type>> DeduceColonRefToBuiltinNameDef(
    BuiltinNameDef* builtin_name_def, const ColonRef* node) {
  const auto& sized_type_keywords = GetSizedTypeKeywordsMetadata();
  if (auto it = sized_type_keywords.find(builtin_name_def->identifier());
      it != sized_type_keywords.end()) {
    auto [is_signed, size] = it->second;
    if (node->attr() == "MAX") {
      return std::make_unique<BitsType>(is_signed, size);
    }
    if (node->attr() == "ZERO") {
      return std::make_unique<BitsType>(is_signed, size);
    }
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Builtin type '%s' does not have attribute '%s'.",
                        builtin_name_def->identifier(), node->attr()));
  }
  return TypeInferenceErrorStatus(
      node->span(), nullptr,
      absl::StrFormat("Builtin '%s' has no attributes.",
                      builtin_name_def->identifier()));
}

static absl::StatusOr<std::unique_ptr<Type>> DeduceColonRefToArrayType(
    ArrayTypeAnnotation* array_type, const ColonRef* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> resolved, ctx->Deduce(array_type));
  XLS_ASSIGN_OR_RETURN(
      resolved,
      UnwrapMetaType(std::move(resolved), array_type->span(), "array type"));
  if (!IsBitsLike(*resolved)) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Cannot use '::' on type %s -- only bits types support "
                        "'::' attributes",
                        resolved->ToString()));
  }
  if (node->attr() != "MAX" && node->attr() != "ZERO") {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("Type '%s' does not have attribute '%s'.",
                        array_type->ToString(), node->attr()));
  }
  return resolved;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceColonRef(const ColonRef* node,
                                                     DeduceCtx* ctx) {
  XLS_VLOG(5) << "Deducing type for ColonRef @ " << node->span().ToString();

  ImportData* import_data = ctx->import_data();
  XLS_ASSIGN_OR_RETURN(auto subject, ResolveColonRefSubjectForTypeChecking(
                                         import_data, ctx->type_info(), node));

  using ReturnT = absl::StatusOr<std::unique_ptr<Type>>;
  Module* subject_module = ToAstNode(subject)->owner();
  XLS_ASSIGN_OR_RETURN(TypeInfo * subject_type_info,
                       import_data->GetRootTypeInfo(subject_module));
  auto subject_ctx = ctx->MakeCtx(subject_type_info, subject_module);
  const FnStackEntry& peek_entry = ctx->fn_stack().back();
  subject_ctx->AddFnStackEntry(peek_entry);
  return absl::visit(
      Visitor{
          [&](Module* module) -> ReturnT {
            return DeduceColonRefToModule(node, module, subject_ctx.get());
          },
          [&](EnumDef* enum_def) -> ReturnT {
            if (!enum_def->HasValue(node->attr())) {
              return TypeInferenceErrorStatus(
                  node->span(), nullptr,
                  absl::StrFormat("Name '%s' is not defined by the enum %s.",
                                  node->attr(), enum_def->identifier()));
            }
            XLS_ASSIGN_OR_RETURN(auto enum_type,
                                 DeduceEnumDef(enum_def, subject_ctx.get()));
            return UnwrapMetaType(std::move(enum_type), node->span(),
                                  "enum type");
          },
          [&](BuiltinNameDef* builtin_name_def) -> ReturnT {
            return DeduceColonRefToBuiltinNameDef(builtin_name_def, node);
          },
          [&](ArrayTypeAnnotation* type) -> ReturnT {
            return DeduceColonRefToArrayType(type, node, subject_ctx.get());
          },
          [&](StructDef* struct_def) -> ReturnT {
            return TypeInferenceErrorStatus(
                node->span(), nullptr,
                absl::StrFormat("Struct definitions (e.g. '%s') cannot have "
                                "constant items.",
                                struct_def->identifier()));
          },
          [&](ColonRef* colon_ref) -> ReturnT {
            // Note: this should be unreachable, as it's a colon-reference that
            // refers *directly* to another colon-ref. Generally you need an
            // intervening construct, like a type alias.
            return absl::InternalError(
                "Colon-reference subject was another colon-reference.");
          },
      },
      subject);
}

// Returns (start, width), resolving indices via DSLX bit slice semantics.
static absl::StatusOr<StartAndWidth> ResolveBitSliceIndices(
    int64_t bit_count, std::optional<int64_t> start_opt,
    std::optional<int64_t> limit_opt) {
  XLS_RET_CHECK_GE(bit_count, 0);
  int64_t start = 0;
  int64_t limit = bit_count;

  if (start_opt.has_value()) {
    start = *start_opt;
  }
  if (limit_opt.has_value()) {
    limit = *limit_opt;
  }

  if (start < 0) {
    start += bit_count;
  }
  if (limit < 0) {
    limit += bit_count;
  }

  limit = std::min(std::max(limit, int64_t{0}), bit_count);
  start = std::min(std::max(start, int64_t{0}), limit);
  XLS_RET_CHECK_GE(start, 0);
  XLS_RET_CHECK_GE(limit, start);
  return StartAndWidth{start, limit - start};
}

static absl::StatusOr<std::unique_ptr<Type>> DeduceWidthSliceType(
    const Index* node, const BitsType& subject_type,
    const WidthSlice& width_slice, DeduceCtx* ctx) {
  // Start expression; e.g. in `x[a+:u4]` this is `a`.
  Expr* start = width_slice.start();

  // Determined type of the start expression (must be bits kind).
  std::unique_ptr<Type> start_type_owned;
  BitsType* start_type;

  if (Number* start_number = dynamic_cast<Number*>(start);
      start_number != nullptr && start_number->type_annotation() == nullptr) {
    // A literal number with no annotated type as the slice start.
    //
    // By default, we use the "subject" type (converted to unsigned) as the type
    // for the slice start.
    start_type_owned = subject_type.ToUBits();
    start_type = dynamic_cast<BitsType*>(start_type_owned.get());

    // Get the start number as an integral value, after we make sure it fits.
    XLS_ASSIGN_OR_RETURN(Bits start_bits, start_number->GetBits(64));
    XLS_ASSIGN_OR_RETURN(int64_t start_int, start_bits.ToInt64());

    if (start_int < 0) {
      return TypeInferenceErrorStatus(
          start_number->span(), nullptr,
          absl::StrFormat("Width-slice start value cannot be negative, only "
                          "unsigned values are permitted; got start value: %d.",
                          start_int));
    }

    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> resolved_start_type,
                         Resolve(*start_type, ctx));
    XLS_ASSIGN_OR_RETURN(TypeDim bit_count_ctd,
                         resolved_start_type->GetTotalBitCount());
    XLS_ASSIGN_OR_RETURN(int64_t bit_count, bit_count_ctd.GetAsInt64());

    // Make sure the start_int literal fits in the type we determined.
    absl::Status fits_status = SBitsWithStatus(start_int, bit_count).status();
    if (!fits_status.ok()) {
      return TypeInferenceErrorStatus(
          node->span(), resolved_start_type.get(),
          absl::StrFormat("Cannot fit slice start %d in %d bits (width "
                          "inferred from slice subject).",
                          start_int, bit_count));
    }
    ctx->type_info()->SetItem(start, *resolved_start_type);
    XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
        ctx->import_data(), ctx->type_info(), ctx->warnings(),
        ctx->GetCurrentParametricEnv(), start_number,
        resolved_start_type.get()));
  } else {
    // Aside from a bare literal (with no type) we should be able to deduce the
    // start expression's type.
    XLS_ASSIGN_OR_RETURN(start_type_owned, ctx->Deduce(start));
    start_type = dynamic_cast<BitsType*>(start_type_owned.get());
    if (start_type == nullptr) {
      return TypeInferenceErrorStatus(
          start->span(), start_type,
          "Start expression for width slice must be bits typed.");
    }
  }

  // Validate that the start is unsigned.
  if (start_type->is_signed()) {
    return TypeInferenceErrorStatus(
        node->span(), start_type,
        "Start index for width-based slice must be unsigned.");
  }

  // If the width of the width_type is bigger than the subject, we flag an
  // error (prevent requesting over-slicing at compile time).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> width_type,
                       ctx->Deduce(width_slice.width()));
  XLS_ASSIGN_OR_RETURN(width_type, UnwrapMetaType(std::move(width_type),
                                                  width_slice.width()->span(),
                                                  "width slice type"));

  XLS_ASSIGN_OR_RETURN(TypeDim width_ctd, width_type->GetTotalBitCount());
  const TypeDim& subject_ctd = subject_type.size();
  if (std::holds_alternative<InterpValue>(width_ctd.value()) &&
      std::holds_alternative<InterpValue>(subject_ctd.value())) {
    XLS_ASSIGN_OR_RETURN(int64_t width_bits, width_ctd.GetAsInt64());
    XLS_ASSIGN_OR_RETURN(int64_t subject_bits, subject_ctd.GetAsInt64());
    if (width_bits > subject_bits) {
      return ctx->TypeMismatchError(
          start->span(), nullptr, subject_type, nullptr, *width_type,
          absl::StrFormat("Slice type must have <= original number of bits; "
                          "attempted slice from %d to %d bits.",
                          subject_bits, width_bits));
    }
  }

  // Validate that the width type is bits-based (e.g. no enums, since sliced
  // value could be out of range of the valid enum values).
  if (dynamic_cast<BitsType*>(width_type.get()) == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), width_type.get(),
        "A bits type is required for a width-based slice.");
  }

  // The width type is the thing returned from the width-slice.
  return width_type;
}

// Attempts to resolve one of the bounds (start or limit) of slice into a
// DSLX-compile-time constant.
static absl::StatusOr<std::optional<int64_t>> TryResolveBound(
    Slice* slice, Expr* bound, std::string_view bound_name, Type* s32,
    const absl::flat_hash_map<std::string, InterpValue>& env, DeduceCtx* ctx) {
  if (bound == nullptr) {
    return std::nullopt;
  }

  absl::StatusOr<InterpValue> bound_or = InterpretExpr(ctx, bound, env);
  if (!bound_or.ok()) {
    const absl::Status& status = bound_or.status();
    if (absl::StrContains(status.message(), "could not find slot or binding")) {
      return TypeInferenceErrorStatus(
          bound->span(), nullptr,
          absl::StrFormat(
              "Unable to resolve slice %s to a compile-time constant.",
              bound_name));
    }
  }

  const InterpValue& value = bound_or.value();
  if (value.tag() != InterpValueTag::kSBits) {  // Error if bound is not signed.
    std::string error_suffix = ".";
    if (value.tag() == InterpValueTag::kUBits) {
      error_suffix = " -- consider casting to a signed value?";
    }
    return TypeInferenceErrorStatus(
        bound->span(), nullptr,
        absl::StrFormat(
            "Slice %s must be a signed compile-time-constant value%s",
            bound_name, error_suffix));
  }

  XLS_ASSIGN_OR_RETURN(int64_t as_64b, value.GetBitValueViaSign());
  XLS_VLOG(3) << absl::StreamFormat("Slice %s bound @ %s has value: %d",
                                    bound_name, bound->span().ToString(),
                                    as_64b);
  return as_64b;
}

// Deduces the concrete type for an Index AST node with a slice spec.
//
// Precondition: node->rhs() is either a Slice or a WidthSlice.
static absl::StatusOr<std::unique_ptr<Type>> DeduceSliceType(
    const Index* node, DeduceCtx* ctx, std::unique_ptr<Type> lhs_type) {
  auto* bits_type = dynamic_cast<BitsType*>(lhs_type.get());
  if (bits_type == nullptr) {
    // TODO(leary): 2019-10-28 Only slicing bits types for now, and only with
    // Number AST nodes, generalize to arrays and constant expressions.
    return TypeInferenceErrorStatus(node->span(), lhs_type.get(),
                                    "Value to slice is not of 'bits' type.");
  }

  if (bits_type->is_signed()) {
    return TypeInferenceErrorStatus(node->span(), lhs_type.get(),
                                    "Bit slice LHS must be unsigned.");
  }

  if (std::holds_alternative<WidthSlice*>(node->rhs())) {
    auto* width_slice = std::get<WidthSlice*>(node->rhs());
    return DeduceWidthSliceType(node, *bits_type, *width_slice, ctx);
  }

  absl::flat_hash_map<std::string, InterpValue> env;
  XLS_ASSIGN_OR_RETURN(
      env,
      MakeConstexprEnv(ctx->import_data(), ctx->type_info(), ctx->warnings(),
                       node, ctx->fn_stack().back().parametric_env()));

  std::unique_ptr<BitsType> s32 = BitsType::MakeS32();
  auto* slice = std::get<Slice*>(node->rhs());

  // Constexpr evaluate start & limit, skipping deducing in the case of
  // undecorated literals.
  auto should_deduce = [](Expr* expr) {
    if (Number* number = dynamic_cast<Number*>(expr);
        number != nullptr && number->type_annotation() == nullptr) {
      return false;
    }
    return true;
  };

  if (slice->start() != nullptr) {
    if (should_deduce(slice->start())) {
      XLS_RETURN_IF_ERROR(Deduce(slice->start(), ctx).status());
    } else {
      // If the slice start is untyped, assume S32, and check it fits in that
      // size.
      XLS_RETURN_IF_ERROR(
          TryEnsureFitsInBitsType(*down_cast<Number*>(slice->start()), *s32));
      ctx->type_info()->SetItem(slice->start(), *s32);
    }
  }
  XLS_ASSIGN_OR_RETURN(
      std::optional<int64_t> start,
      TryResolveBound(slice, slice->start(), "start", s32.get(), env, ctx));

  if (slice->limit() != nullptr) {
    if (should_deduce(slice->limit())) {
      XLS_RETURN_IF_ERROR(Deduce(slice->limit(), ctx).status());
    } else {
      // If the slice limit is untyped, assume S32, and check it fits in that
      // size.
      XLS_RETURN_IF_ERROR(
          TryEnsureFitsInBitsType(*down_cast<Number*>(slice->limit()), *s32));
      ctx->type_info()->SetItem(slice->limit(), *s32);
    }
  }
  XLS_ASSIGN_OR_RETURN(
      std::optional<int64_t> limit,
      TryResolveBound(slice, slice->limit(), "limit", s32.get(), env, ctx));

  const ParametricEnv& fn_parametric_env =
      ctx->fn_stack().back().parametric_env();
  XLS_ASSIGN_OR_RETURN(TypeDim lhs_bit_count_ctd, lhs_type->GetTotalBitCount());
  int64_t bit_count;
  if (std::holds_alternative<TypeDim::OwnedParametric>(
          lhs_bit_count_ctd.value())) {
    auto& owned_parametric =
        std::get<TypeDim::OwnedParametric>(lhs_bit_count_ctd.value());
    ParametricExpression::Evaluated evaluated =
        owned_parametric->Evaluate(ToParametricEnv(fn_parametric_env));
    InterpValue v = std::get<InterpValue>(evaluated);
    bit_count = v.GetBitValueViaSign().value();
  } else {
    XLS_ASSIGN_OR_RETURN(bit_count, lhs_bit_count_ctd.GetAsInt64());
  }
  XLS_ASSIGN_OR_RETURN(StartAndWidth saw,
                       ResolveBitSliceIndices(bit_count, start, limit));
  ctx->type_info()->AddSliceStartAndWidth(slice, fn_parametric_env, saw);

  // Make sure the start and end types match and that the limit fits.
  std::unique_ptr<Type> start_type;
  std::unique_ptr<Type> limit_type;
  if (slice->start() == nullptr && slice->limit() == nullptr) {
    start_type = BitsType::MakeS32();
    limit_type = BitsType::MakeS32();
  } else if (slice->start() != nullptr && slice->limit() == nullptr) {
    XLS_ASSIGN_OR_RETURN(BitsType * tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->start()));
    start_type = tmp->CloneToUnique();
    limit_type = start_type->CloneToUnique();
  } else if (slice->start() == nullptr && slice->limit() != nullptr) {
    XLS_ASSIGN_OR_RETURN(BitsType * tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->limit()));
    limit_type = tmp->CloneToUnique();
    start_type = limit_type->CloneToUnique();
  } else {
    XLS_ASSIGN_OR_RETURN(BitsType * tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->start()));
    start_type = tmp->CloneToUnique();
    XLS_ASSIGN_OR_RETURN(tmp,
                         ctx->type_info()->GetItemAs<BitsType>(slice->limit()));
    limit_type = tmp->CloneToUnique();
  }

  if (*start_type != *limit_type) {
    return TypeInferenceErrorStatus(
        node->span(), limit_type.get(),
        absl::StrFormat(
            "Slice limit type (%s) did not match slice start type (%s).",
            limit_type->ToString(), start_type->ToString()));
  }
  XLS_ASSIGN_OR_RETURN(TypeDim type_width_dim, start_type->GetTotalBitCount());
  XLS_ASSIGN_OR_RETURN(int64_t type_width, type_width_dim.GetAsInt64());
  if (Bits::MinBitCountSigned(saw.start + saw.width) > type_width) {
    return TypeInferenceErrorStatus(
        node->span(), limit_type.get(),
        absl::StrFormat("Slice limit does not fit in index type: %d.",
                        saw.start + saw.width));
  }

  return std::make_unique<BitsType>(/*signed=*/false, saw.width);
}

absl::StatusOr<std::unique_ptr<Type>> DeduceIndex(const Index* node,
                                                  DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> lhs_type,
                       ctx->Deduce(node->lhs()));

  if (std::holds_alternative<Slice*>(node->rhs()) ||
      std::holds_alternative<WidthSlice*>(node->rhs())) {
    return DeduceSliceType(node, ctx, std::move(lhs_type));
  }
  Expr* rhs = std::get<Expr*>(node->rhs());

  if (auto* tuple_type = dynamic_cast<TupleType*>(lhs_type.get())) {
    return TypeInferenceErrorStatus(
        node->span(), tuple_type,
        "Tuples should not be indexed with array-style syntax. "
        "Use `tuple.<number>` syntax instead.");
  }

  auto* array_type = dynamic_cast<ArrayType*>(lhs_type.get());
  if (array_type == nullptr) {
    return TypeInferenceErrorStatus(node->span(), lhs_type.get(),
                                    "Value to index is not an array.");
  }

  ctx->set_in_typeless_number_ctx(true);
  absl::Cleanup cleanup = [ctx]() { ctx->set_in_typeless_number_ctx(false); };

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> index_type,
                       ctx->Deduce(ToAstNode(rhs)));
  XLS_RET_CHECK(index_type != nullptr);

  auto* index_bits = dynamic_cast<BitsType*>(index_type.get());
  if (index_bits == nullptr || index_bits->is_signed()) {
    return TypeInferenceErrorStatus(node->span(), index_type.get(),
                                    "Index is not unsigned-bits typed.");
  }

  XLS_VLOG(10) << absl::StreamFormat("Index RHS: `%s` constexpr? %d",
                                     rhs->ToString(),
                                     ctx->type_info()->IsKnownConstExpr(rhs));

  // If we know the array size concretely and the index is a constexpr
  // expression, we can check it is in bounds.
  //
  // TODO(leary): 2024-02-29 Check this in the various slice forms that are not
  // expressions.
  if (!array_type->size().IsParametric() &&
      ctx->type_info()->IsKnownConstExpr(rhs)) {
    XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                         ctx->type_info()->GetConstExpr(rhs));
    XLS_VLOG(10) << "Index RHS is known constexpr value: " << constexpr_value;
    XLS_ASSIGN_OR_RETURN(uint64_t constexpr_index,
                         constexpr_value.GetBitValueUnsigned());
    XLS_ASSIGN_OR_RETURN(int64_t array_size, array_type->size().GetAsInt64());
    if (constexpr_index >= array_size) {
      return TypeInferenceErrorStatus(
          node->span(), array_type,
          absl::StrFormat("Index has a compile-time constant value %d that is "
                          "out of bounds of the array type.",
                          constexpr_index));
    }
  }

  return array_type->element_type().CloneToUnique();
}

// Ensures that the name_def_tree bindings are aligned with the type "other"
// (which is the type for the matched value at this name_def_tree level).
static absl::Status Unify(NameDefTree* name_def_tree, const Type& other,
                          DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> resolved_rhs_type,
                       Resolve(other, ctx));
  if (name_def_tree->is_leaf()) {
    NameDefTree::Leaf leaf = name_def_tree->leaf();
    if (std::holds_alternative<NameDef*>(leaf)) {
      // Defining a name in the pattern match, we accept all types.
      ctx->type_info()->SetItem(ToAstNode(leaf), *resolved_rhs_type);
    } else if (std::holds_alternative<WildcardPattern*>(leaf)) {
      // Nothing to do.
    } else if (std::holds_alternative<Number*>(leaf) ||
               std::holds_alternative<ColonRef*>(leaf)) {
      // For a reference (or literal) the types must be consistent.
      XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> resolved_leaf_type,
                           DeduceAndResolve(ToAstNode(leaf), ctx));
      if (*resolved_leaf_type != *resolved_rhs_type) {
        return ctx->TypeMismatchError(
            name_def_tree->span(), nullptr, *resolved_rhs_type, nullptr,
            *resolved_leaf_type,
            absl::StrFormat(
                "Conflicting types; pattern expects %s but got %s from value",
                resolved_rhs_type->ToString(), resolved_leaf_type->ToString()));
      }
    }
  } else {
    const NameDefTree::Nodes& nodes = name_def_tree->nodes();
    auto* type = dynamic_cast<const TupleType*>(&other);
    if (type == nullptr) {
      return TypeInferenceErrorStatus(
          name_def_tree->span(), &other,
          "Pattern expected matched-on type to be a tuple.");
    }
    if (type->size() != nodes.size()) {
      return TypeInferenceErrorStatus(
          name_def_tree->span(), &other,
          absl::StrFormat("Pattern wanted %d tuple elements, matched-on "
                          "value had %d element",
                          nodes.size(), type->size()));
    }
    for (int64_t i = 0; i < nodes.size(); ++i) {
      const Type& subtype = type->GetMemberType(i);
      NameDefTree* subtree = nodes[i];
      XLS_RETURN_IF_ERROR(Unify(subtree, subtype, ctx));
    }
  }
  return absl::OkStatus();
}

static std::string PatternsToString(MatchArm* arm) {
  return absl::StrJoin(arm->patterns(), " | ",
                       [](std::string* out, NameDefTree* ndt) {
                         absl::StrAppend(out, ndt->ToString());
                       });
}

absl::StatusOr<std::unique_ptr<Type>> DeduceMatch(const Match* node,
                                                  DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> matched,
                       ctx->Deduce(node->matched()));

  if (node->arms().empty()) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        "Match construct has no arms, cannot determine its type.");
  }

  absl::flat_hash_set<std::string> seen_patterns;
  for (MatchArm* arm : node->arms()) {
    // We opportunistically identify syntactically identical match arms -- this
    // is a user error since the first should always match, the latter is
    // totally redundant.
    std::string patterns_string = PatternsToString(arm);
    if (auto [it, inserted] = seen_patterns.insert(patterns_string);
        !inserted) {
      return TypeInferenceErrorStatus(
          arm->GetPatternSpan(), nullptr,
          absl::StrFormat("Exact-duplicate pattern match detected `%s` -- only "
                          "the first could possibly match",
                          patterns_string));
    }

    for (NameDefTree* pattern : arm->patterns()) {
      // Deduce types for all patterns with types that can be checked.
      //
      // Note that NameDef is handled in the Unify() call below, and
      // WildcardPattern has no type because it's a black hole.
      for (NameDefTree::Leaf leaf : pattern->Flatten()) {
        if (!std::holds_alternative<NameDef*>(leaf) &&
            !std::holds_alternative<WildcardPattern*>(leaf)) {
          XLS_RETURN_IF_ERROR(ctx->Deduce(ToAstNode(leaf)).status());
        }
      }

      XLS_RETURN_IF_ERROR(Unify(pattern, *matched, ctx));
    }
  }

  std::vector<std::unique_ptr<Type>> arm_types;
  for (MatchArm* arm : node->arms()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> arm_type,
                         DeduceAndResolve(arm, ctx));
    arm_types.push_back(std::move(arm_type));
  }

  for (int64_t i = 1; i < arm_types.size(); ++i) {
    if (*arm_types[i] != *arm_types[0]) {
      return ctx->TypeMismatchError(
          node->arms()[i]->span(), nullptr, *arm_types[i], nullptr,
          *arm_types[0],
          "This match arm did not have the same type as the "
          "preceding match arms.");
    }
  }

  return std::move(arm_types[0]);
}

struct ValidatedStructMembers {
  // Names seen in the struct instance; e.g. for a SplatStructInstance can be a
  // subset of the struct member names.
  //
  // Note: we use a btree set so we can do set differencing via c_set_difference
  // (which works on ordered sets).
  absl::btree_set<std::string> seen_names;

  std::vector<InstantiateArg> args;
  std::vector<std::unique_ptr<Type>> member_types;
};

// Validates a struct instantiation is a subset of 'members' with no dups.
//
// Args:
//  members: Sequence of members used in instantiation. Note this may be a
//    subset; e.g. in the case of splat instantiation.
//  struct_type: The deduced type for the struct (instantiation).
//  struct_text: Display name to use for the struct in case of an error.
//  ctx: Wrapper containing node to type mapping context.
//
// Returns:
//  A tuple containing:
//  * The set of struct member names that were instantiated
//  * The Types of the provided arguments
//  * The Types of the corresponding struct member definition.
static absl::StatusOr<ValidatedStructMembers> ValidateStructMembersSubset(
    absl::Span<const std::pair<std::string, Expr*>> members,
    const StructType& struct_type, std::string_view struct_text,
    DeduceCtx* ctx) {
  ValidatedStructMembers result;
  for (auto& [name, expr] : members) {
    if (!result.seen_names.insert(name).second) {
      return TypeInferenceErrorStatus(
          expr->span(), nullptr,
          absl::StrFormat(
              "Duplicate value seen for '%s' in this '%s' struct instance.",
              name, struct_text));
    }
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> expr_type,
                         DeduceAndResolve(expr, ctx));
    XLS_RET_CHECK(!expr_type->IsMeta())
        << "name: " << name << " expr: " << expr->ToString()
        << " type: " << expr_type->ToString();

    result.args.push_back(InstantiateArg{std::move(expr_type), expr->span()});
    std::optional<const Type*> maybe_type =
        struct_type.GetMemberTypeByName(name);

    if (maybe_type.has_value()) {
      XLS_RET_CHECK(!maybe_type.value()->IsMeta())
          << maybe_type.value()->ToString();
      result.member_types.push_back(maybe_type.value()->CloneToUnique());
    } else {
      return TypeInferenceErrorStatus(
          expr->span(), nullptr,
          absl::StrFormat("Struct '%s' has no member '%s', but it was provided "
                          "by this instance.",
                          struct_text, name));
    }
  }

  return result;
}

// Dereferences the "original" struct reference to a struct definition or
// returns an error.
//
// Args:
//  span: The span of the original construct trying to dereference the struct
//    (e.g. a StructInstance).
//  original: The original struct reference value (used in error reporting).
//  current: The current type definition being dereferenced towards a struct
//    definition (note there can be multiple levels of typedefs and such).
//  type_info: The type information that the "current" TypeDefinition resolves
//    against.
static absl::StatusOr<StructDef*> DerefToStruct(
    const Span& span, std::string_view original_ref_text,
    TypeDefinition current, TypeInfo* type_info) {
  while (true) {
    if (std::holds_alternative<StructDef*>(current)) {  // Done dereferencing.
      return std::get<StructDef*>(current);
    }
    if (std::holds_alternative<TypeAlias*>(current)) {
      auto* type_alias = std::get<TypeAlias*>(current);
      TypeAnnotation* annotation = type_alias->type_annotation();
      TypeRefTypeAnnotation* type_ref =
          dynamic_cast<TypeRefTypeAnnotation*>(annotation);
      if (type_ref == nullptr) {
        return TypeInferenceErrorStatus(
            span, nullptr,
            absl::StrFormat("Could not resolve struct from %s; found: %s @ %s",
                            original_ref_text, annotation->ToString(),
                            annotation->span().ToString()));
      }
      current = type_ref->type_ref()->type_definition();
      continue;
    }
    if (std::holds_alternative<ColonRef*>(current)) {
      auto* colon_ref = std::get<ColonRef*>(current);
      // Colon ref has to be dereferenced, may be a module reference.
      ColonRef::Subject subject = colon_ref->subject();
      // TODO(leary): 2020-12-12 Original logic was this way, but we should be
      // able to violate this assertion.
      XLS_RET_CHECK(std::holds_alternative<NameRef*>(subject));
      auto* name_ref = std::get<NameRef*>(subject);
      AnyNameDef any_name_def = name_ref->name_def();
      XLS_RET_CHECK(std::holds_alternative<const NameDef*>(any_name_def));
      const NameDef* name_def = std::get<const NameDef*>(any_name_def);
      AstNode* definer = name_def->definer();
      auto* import = dynamic_cast<Import*>(definer);
      if (import == nullptr) {
        return TypeInferenceErrorStatus(
            span, nullptr,
            absl::StrFormat("Could not resolve struct from %s; found: %s @ %s",
                            original_ref_text, name_ref->ToString(),
                            name_ref->span().ToString()));
      }
      std::optional<const ImportedInfo*> imported =
          type_info->GetImported(import);
      XLS_RET_CHECK(imported.has_value());
      Module* module = imported.value()->module;
      XLS_ASSIGN_OR_RETURN(current,
                           module->GetTypeDefinition(colon_ref->attr()));
      return DerefToStruct(span, original_ref_text, current,
                           imported.value()->type_info);
    }
    XLS_RET_CHECK(std::holds_alternative<EnumDef*>(current));
    auto* enum_def = std::get<EnumDef*>(current);
    return TypeInferenceErrorStatus(
        span, nullptr,
        absl::StrFormat("Expected struct reference, but found enum: %s",
                        enum_def->identifier()));
  }
}

// Wrapper around the DerefToStruct above (that works on TypeDefinitions) that
// takes a `TypeAnnotation` instead.
static absl::StatusOr<StructDef*> DerefToStruct(
    const Span& span, std::string_view original_ref_text,
    TypeAnnotation* type_annotation, TypeInfo* type_info) {
  auto* type_ref_type_annotation =
      dynamic_cast<TypeRefTypeAnnotation*>(type_annotation);
  if (type_ref_type_annotation == nullptr) {
    return TypeInferenceErrorStatus(
        span, nullptr,
        absl::StrFormat("Could not resolve struct from %s (%s) @ %s",
                        type_annotation->ToString(),
                        type_annotation->GetNodeTypeName(),
                        type_annotation->span().ToString()));
  }

  return DerefToStruct(span, original_ref_text,
                       type_ref_type_annotation->type_ref()->type_definition(),
                       type_info);
}

// Deduces the type for a ParametricBinding (via its type annotation).
//
// Note that this returns the type of the expression (i.e. parameter reference)
// not the metatype (i.e. not the type of the type-annotation).
static absl::StatusOr<std::unique_ptr<Type>> ParametricBindingToType(
    ParametricBinding* binding, DeduceCtx* ctx) {
  Module* binding_module = binding->owner();
  ImportData* import_data = ctx->import_data();
  XLS_ASSIGN_OR_RETURN(TypeInfo * binding_type_info,
                       import_data->GetRootTypeInfo(binding_module));
  auto binding_ctx = ctx->MakeCtx(binding_type_info, binding_module);
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type,
                       binding_ctx->Deduce(binding->type_annotation()));
  return UnwrapMetaType(std::move(type), binding->type_annotation()->span(),
                        "parametric binding type");
}

absl::StatusOr<std::unique_ptr<Type>> DeduceStructInstance(
    const StructInstance* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "Deducing type for struct instance: " << node->ToString();

  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type,
                       ctx->Deduce(ToAstNode(node->struct_ref())));
  XLS_ASSIGN_OR_RETURN(type, UnwrapMetaType(std::move(type), node->span(),
                                            "struct instance type"));

  auto* struct_type = dynamic_cast<const StructType*>(type.get());
  if (struct_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), struct_type,
        "Expected a struct definition to instantiate");
  }

  // Note what names we expect to be present.
  XLS_ASSIGN_OR_RETURN(std::vector<std::string> names,
                       struct_type->GetMemberNames());
  absl::btree_set<std::string> expected_names(names.begin(), names.end());

  XLS_ASSIGN_OR_RETURN(
      ValidatedStructMembers validated,
      ValidateStructMembersSubset(node->GetUnorderedMembers(), *struct_type,
                                  node->struct_ref()->ToString(), ctx));
  if (validated.seen_names != expected_names) {
    absl::btree_set<std::string> missing_set;
    absl::c_set_difference(expected_names, validated.seen_names,
                           std::inserter(missing_set, missing_set.begin()));
    std::vector<std::string> missing(missing_set.begin(), missing_set.end());
    std::sort(missing.begin(), missing.end());
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat(
            "Struct instance is missing member(s): %s",
            absl::StrJoin(missing, ", ",
                          [](std::string* out, const std::string& piece) {
                            absl::StrAppendFormat(out, "'%s'", piece);
                          })));
  }

  TypeAnnotation* struct_ref = node->struct_ref();
  XLS_ASSIGN_OR_RETURN(StructDef * struct_def,
                       DerefToStruct(node->span(), struct_ref->ToString(),
                                     struct_ref, ctx->type_info()));

  XLS_ASSIGN_OR_RETURN(
      std::vector<ParametricConstraint> parametric_constraints,
      ParametricBindingsToConstraints(struct_def->parametric_bindings(), ctx));
  XLS_ASSIGN_OR_RETURN(
      TypeAndParametricEnv tab,
      InstantiateStruct(node->span(), *struct_type, validated.args,
                        validated.member_types, ctx, parametric_constraints));

  return std::move(tab.type);
}

absl::StatusOr<std::unique_ptr<Type>> DeduceSplatStructInstance(
    const SplatStructInstance* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> struct_type_ct,
                       ctx->Deduce(ToAstNode(node->struct_ref())));
  XLS_ASSIGN_OR_RETURN(struct_type_ct,
                       UnwrapMetaType(std::move(struct_type_ct), node->span(),
                                      "splatted struct instance type"));

  // The type of the splatted value; e.g. in `MyStruct{..s}` the type of `s`.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> splatted_type_ct,
                       ctx->Deduce(node->splatted()));

  // The splatted type should be (nominally) equivalent to the struct type,
  // because that's where we're filling in the default values from (those values
  // that were not directly provided by the user).
  auto* struct_type = dynamic_cast<StructType*>(struct_type_ct.get());
  if (struct_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->struct_ref()->span(), struct_type_ct.get(),
        absl::StrFormat("Type given to struct instantiation was not a struct"));
  }

  auto* splatted_type = dynamic_cast<StructType*>(splatted_type_ct.get());
  if (splatted_type == nullptr) {
    return TypeInferenceErrorStatus(
        node->splatted()->span(), splatted_type_ct.get(),
        absl::StrFormat(
            "Type given to 'splatted' struct instantiation was not a struct"));
  }

  if (&struct_type->nominal_type() != &splatted_type->nominal_type()) {
    return ctx->TypeMismatchError(
        node->span(), nullptr, *struct_type, nullptr, *splatted_type,
        absl::StrFormat("Attempting to fill values in '%s' instantiation from "
                        "a value of type '%s'",
                        struct_type->nominal_type().identifier(),
                        splatted_type->nominal_type().identifier()));
  }

  XLS_ASSIGN_OR_RETURN(
      ValidatedStructMembers validated,
      ValidateStructMembersSubset(node->members(), *struct_type,
                                  node->struct_ref()->ToString(), ctx));

  XLS_ASSIGN_OR_RETURN(std::vector<std::string> all_names,
                       struct_type->GetMemberNames());
  XLS_VLOG(5) << "SplatStructInstance @ " << node->span() << " seen names: ["
              << absl::StrJoin(validated.seen_names, ", ") << "] "
              << " all names: [" << absl::StrJoin(all_names, ", ") << "]";

  if (validated.seen_names.size() == all_names.size()) {
    ctx->warnings()->Add(
        node->splatted()->span(), WarningKind::kUselessStructSplat,
        absl::StrFormat("'Splatted' struct instance has all members of struct "
                        "defined, consider removing the `..%s`",
                        node->splatted()->ToString()));
  }

  for (const std::string& name : all_names) {
    // If we didn't see the name, it comes from the "splatted" argument.
    if (!validated.seen_names.contains(name)) {
      const Type& splatted_member_type =
          *splatted_type->GetMemberTypeByName(name).value();
      const Type& struct_member_type =
          *struct_type->GetMemberTypeByName(name).value();

      validated.args.push_back(InstantiateArg{
          splatted_member_type.CloneToUnique(), node->splatted()->span()});
      validated.member_types.push_back(struct_member_type.CloneToUnique());
    }
  }

  // At this point, we should have the same number of args compared to the
  // number of members defined in the struct.
  XLS_RET_CHECK_EQ(validated.args.size(), validated.member_types.size());

  TypeAnnotation* struct_ref = node->struct_ref();
  XLS_ASSIGN_OR_RETURN(StructDef * struct_def,
                       DerefToStruct(node->span(), struct_ref->ToString(),
                                     struct_ref, ctx->type_info()));

  XLS_ASSIGN_OR_RETURN(
      std::vector<ParametricConstraint> parametric_constraints,
      ParametricBindingsToConstraints(struct_def->parametric_bindings(), ctx));
  XLS_ASSIGN_OR_RETURN(
      TypeAndParametricEnv tab,
      InstantiateStruct(node->span(), *struct_type, validated.args,
                        validated.member_types, ctx, parametric_constraints));

  return std::move(tab.type);
}

absl::StatusOr<std::unique_ptr<Type>> DeduceBuiltinTypeAnnotation(
    const BuiltinTypeAnnotation* node, DeduceCtx* ctx) {
  std::unique_ptr<Type> t;
  if (node->builtin_type() == BuiltinType::kToken) {
    t = std::make_unique<TokenType>();
  } else {
    t = std::make_unique<BitsType>(node->GetSignedness(), node->GetBitCount());
  }
  return std::make_unique<MetaType>(std::move(t));
}

// Converts an AST expression in "dimension position" (e.g. in an array type
// annotation's size) and converts it into a TypeDim value that can be
// used in (e.g.) a ConcreteStruct. The result is either a constexpr-evaluated
// value or a ParametricSymbol (for a parametric binding that has not yet been
// defined).
//
// Note: this is not capable of expressing more complex ASTs -- it assumes
// something is either fully constexpr-evaluatable, or symbolic.
static absl::StatusOr<TypeDim> DimToConcreteUsize(const Expr* dim_expr,
                                                  DeduceCtx* ctx) {
  std::unique_ptr<BitsType> u32 = BitsType::MakeU32();
  auto validate_high_bit = [&u32](const Span& span, uint32_t value) {
    if ((value >> 31) == 0) {
      return absl::OkStatus();
    }
    return TypeInferenceErrorStatus(
        span, u32.get(),
        absl::StrFormat("Dimension value is too large, high bit is set: %#x; "
                        "was a negative number accidentally cast to a size?",
                        value));
  };

  // We allow numbers in dimension position to go without type annotations -- we
  // implicitly make the type of the dimension u32, as we generally do with
  // dimension values.
  if (auto* number = dynamic_cast<const Number*>(dim_expr)) {
    if (number->type_annotation() == nullptr) {
      XLS_RETURN_IF_ERROR(TryEnsureFitsInBitsType(*number, *u32));
      ctx->type_info()->SetItem(number, *u32);
    } else {
      XLS_ASSIGN_OR_RETURN(auto dim_type, ctx->Deduce(number));
      if (*dim_type != *u32) {
        return ctx->TypeMismatchError(
            dim_expr->span(), nullptr, *dim_type, nullptr, *u32,
            absl::StrFormat(
                "Dimension %s must be a `u32` (soon to be `usize`, see "
                "https://github.com/google/xls/issues/450 for details).",
                dim_expr->ToString()));
      }
    }

    XLS_ASSIGN_OR_RETURN(int64_t value, number->GetAsUint64());
    const uint32_t value_u32 = static_cast<uint32_t>(value);
    XLS_RET_CHECK_EQ(value, value_u32);

    XLS_RETURN_IF_ERROR(validate_high_bit(number->span(), value_u32));

    // No need to use the ConstexprEvaluator here. We've already got the goods.
    // It'd have trouble anyway, since this number isn't type-decorated.
    ctx->type_info()->NoteConstExpr(dim_expr, InterpValue::MakeU32(value_u32));
    return TypeDim::CreateU32(value_u32);
  }

  // First we check that it's a u32 (in the future we'll want it to be a usize).
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> dim_type, ctx->Deduce(dim_expr));
  if (*dim_type != *u32) {
    return ctx->TypeMismatchError(
        dim_expr->span(), nullptr, *dim_type, nullptr, *u32,
        absl::StrFormat(
            "Dimension %s must be a `u32` (soon to be `usize`, see "
            "https://github.com/google/xls/issues/450 for details).",
            dim_expr->ToString()));
  }

  // Now we try to constexpr evaluate it.
  const ParametricEnv parametric_env = ctx->GetCurrentParametricEnv();
  XLS_VLOG(5) << "Attempting to evaluate dimension expression: `"
              << dim_expr->ToString()
              << "` via parametric env: " << parametric_env;
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(), parametric_env,
      dim_expr, dim_type.get()));
  if (ctx->type_info()->IsKnownConstExpr(dim_expr)) {
    XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                         ctx->type_info()->GetConstExpr(dim_expr));
    XLS_ASSIGN_OR_RETURN(uint64_t int_value,
                         constexpr_value.GetBitValueViaSign());
    uint32_t u32_value = static_cast<uint32_t>(int_value);
    XLS_RETURN_IF_ERROR(validate_high_bit(dim_expr->span(), u32_value));
    XLS_RET_CHECK_EQ(u32_value, int_value);
    return TypeDim::CreateU32(u32_value);
  }

  // If there wasn't a known constexpr we could evaluate it to at this point, we
  // attempt to turn it into a parametric expression.
  absl::StatusOr<std::unique_ptr<ParametricExpression>> parametric_expr_or =
      ExprToParametric(dim_expr, ctx);
  if (parametric_expr_or.ok()) {
    return TypeDim(std::move(parametric_expr_or).value());
  }

  XLS_VLOG(3) << "Could not convert dim expr to parametric expr; status: "
              << parametric_expr_or.status();

  // If we can't evaluate it to a parametric expression we give an error.
  return TypeInferenceErrorStatus(
      dim_expr->span(), nullptr,
      absl::StrFormat(
          "Could not evaluate dimension expression `%s` to a constant value.",
          dim_expr->ToString()));
}

// As above, but converts to a TypeDim value that is boolean.
static absl::StatusOr<TypeDim> DimToConcreteBool(const Expr* dim_expr,
                                                 DeduceCtx* ctx) {
  std::unique_ptr<BitsType> u1 = BitsType::MakeU1();

  // We allow numbers in dimension position to go without type annotations -- we
  // implicitly make the type of the dimension u32, as we generally do with
  // dimension values.
  if (auto* number = dynamic_cast<const Number*>(dim_expr)) {
    if (number->type_annotation() == nullptr) {
      XLS_RETURN_IF_ERROR(TryEnsureFitsInBitsType(*number, *u1));
      ctx->type_info()->SetItem(number, *u1);
    } else {
      XLS_ASSIGN_OR_RETURN(auto dim_type, ctx->Deduce(number));
      if (*dim_type != *u1) {
        return ctx->TypeMismatchError(
            dim_expr->span(), nullptr, *dim_type, nullptr, *u1,
            absl::StrFormat("Dimension %s must be a `bool`/`u1`.",
                            dim_expr->ToString()));
      }
    }

    XLS_ASSIGN_OR_RETURN(int64_t value, number->GetAsUint64());
    const bool value_bool = static_cast<bool>(value);
    XLS_RET_CHECK_EQ(value, value_bool);

    // No need to use the ConstexprEvaluator here. We've already got the goods.
    // It'd have trouble anyway, since this number isn't type-decorated.
    ctx->type_info()->NoteConstExpr(dim_expr,
                                    InterpValue::MakeBool(value_bool));
    return TypeDim::CreateBool(value_bool);
  }

  // First we check that it's a u1.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> dim_type, ctx->Deduce(dim_expr));
  if (*dim_type != *u1) {
    return ctx->TypeMismatchError(
        dim_expr->span(), nullptr, *dim_type, nullptr, *u1,
        absl::StrFormat("Dimension %s must be a `u1`.", dim_expr->ToString()));
  }

  // Now we try to constexpr evaluate it.
  const ParametricEnv parametric_env = ctx->GetCurrentParametricEnv();
  XLS_VLOG(5) << "Attempting to evaluate dimension expression: `"
              << dim_expr->ToString()
              << "` via parametric env: " << parametric_env;
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(), parametric_env,
      dim_expr, dim_type.get()));
  if (ctx->type_info()->IsKnownConstExpr(dim_expr)) {
    XLS_ASSIGN_OR_RETURN(InterpValue constexpr_value,
                         ctx->type_info()->GetConstExpr(dim_expr));
    XLS_ASSIGN_OR_RETURN(uint64_t int_value,
                         constexpr_value.GetBitValueViaSign());
    bool bool_value = static_cast<bool>(int_value);
    XLS_RET_CHECK_EQ(bool_value, int_value);
    return TypeDim::CreateBool(bool_value);
  }

  // If there wasn't a known constexpr we could evaluate it to at this point, we
  // attempt to turn it into a parametric expression.
  absl::StatusOr<std::unique_ptr<ParametricExpression>> parametric_expr_or =
      ExprToParametric(dim_expr, ctx);
  if (parametric_expr_or.ok()) {
    return TypeDim(std::move(parametric_expr_or).value());
  }

  XLS_VLOG(3) << "Could not convert dim expr to parametric expr; status: "
              << parametric_expr_or.status();

  // If we can't evaluate it to a parametric expression we give an error.
  return TypeInferenceErrorStatus(
      dim_expr->span(), nullptr,
      absl::StrFormat(
          "Could not evaluate dimension expression `%s` to a constant value.",
          dim_expr->ToString()));
}

absl::StatusOr<std::unique_ptr<Type>> DeduceChannelTypeAnnotation(
    const ChannelTypeAnnotation* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceChannelTypeAnnotation; node: " << node->ToString();
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> payload_type,
                       Deduce(node->payload(), ctx));
  XLS_RET_CHECK(payload_type->IsMeta())
      << node->payload()->ToString() << " @ " << node->payload()->span();
  XLS_ASSIGN_OR_RETURN(payload_type, UnwrapMetaType(std::move(payload_type),
                                                    node->payload()->span(),
                                                    "channel type annotation"));
  std::unique_ptr<Type> node_type =
      std::make_unique<ChannelType>(std::move(payload_type), node->direction());
  if (node->dims().has_value()) {
    std::vector<Expr*> dims = node->dims().value();

    for (const auto& dim : dims) {
      XLS_ASSIGN_OR_RETURN(TypeDim concrete_dim, DimToConcreteUsize(dim, ctx));
      node_type =
          std::make_unique<ArrayType>(std::move(node_type), concrete_dim);
    }
  }

  return std::make_unique<MetaType>(std::move(node_type));
}

absl::StatusOr<std::unique_ptr<Type>> DeduceTupleTypeAnnotation(
    const TupleTypeAnnotation* node, DeduceCtx* ctx) {
  std::vector<std::unique_ptr<Type>> members;
  for (TypeAnnotation* member : node->members()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type, ctx->Deduce(member));
    XLS_ASSIGN_OR_RETURN(type, UnwrapMetaType(std::move(type), member->span(),
                                              "tuple type member"));
    members.push_back(std::move(type));
  }
  auto t = std::make_unique<TupleType>(std::move(members));
  return std::make_unique<MetaType>(std::move(t));
}

absl::StatusOr<std::unique_ptr<Type>> DeduceArrayTypeAnnotation(
    const ArrayTypeAnnotation* node, DeduceCtx* ctx) {
  XLS_VLOG(5) << "DeduceArrayTypeAnnotation; node: " << node->ToString();

  std::unique_ptr<Type> t;
  if (auto* element_type =
          dynamic_cast<BuiltinTypeAnnotation*>(node->element_type());
      element_type != nullptr && element_type->GetBitCount() == 0) {
    XLS_VLOG(5) << "DeduceArrayTypeAnnotation; bits type constructor: "
                << node->ToString();

    std::optional<TypeDim> dim;
    if (element_type->builtin_type() == BuiltinType::kXN) {
      // This type constructor takes a boolean as its first array argument to
      // indicate signedness.
      XLS_ASSIGN_OR_RETURN(dim, DimToConcreteBool(node->dim(), ctx));
      t = std::make_unique<BitsConstructorType>(std::move(dim).value());
    } else {
      XLS_ASSIGN_OR_RETURN(dim, DimToConcreteUsize(node->dim(), ctx));
      t = std::make_unique<BitsType>(element_type->GetSignedness(),
                                     std::move(dim).value());
    }
  } else {
    XLS_VLOG(5) << "DeduceArrayTypeAnnotation; element_type: "
                << node->element_type()->ToString();
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> e,
                         ctx->Deduce(node->element_type()));
    XLS_ASSIGN_OR_RETURN(
        e, UnwrapMetaType(std::move(e), node->element_type()->span(),
                          "array element type position"));
    XLS_ASSIGN_OR_RETURN(TypeDim dim, DimToConcreteUsize(node->dim(), ctx));
    t = std::make_unique<ArrayType>(std::move(e), std::move(dim));
    XLS_VLOG(4) << absl::StreamFormat("Array type annotation: %s => %s",
                                      node->ToString(), t->ToString());
  }
  auto result = std::make_unique<MetaType>(std::move(t));
  return result;
}

// Returns concretized struct type using the provided bindings.
//
// For example, if we have a struct defined as `struct Foo<N: u32, M: u32>`,
// the default TupleType will be (N, M). If a type annotation provides bindings,
// (e.g. `Foo<A, 16>`), we will replace N, M with those values. In the case
// above, we will return `(A, 16)` instead.
//
// Args:
//   type_annotation: The provided type annotation for this parametric struct.
//   struct_def: The struct definition AST node.
//   base_type: The TupleType of the struct, based only on the struct
//    definition (before parametrics are applied).
static absl::StatusOr<std::unique_ptr<Type>> ConcretizeStructAnnotation(
    const TypeRefTypeAnnotation* type_annotation, const StructDef* struct_def,
    const Type& base_type, DeduceCtx* ctx) {
  XLS_VLOG(5) << "ConcreteStructAnnotation; type_annotation: "
              << type_annotation->ToString()
              << " struct_def: " << struct_def->ToString();

  // Note: if there are too *few* annotated parametrics, some of them may be
  // derived.
  if (type_annotation->parametrics().size() >
      struct_def->parametric_bindings().size()) {
    return TypeInferenceErrorStatus(
        type_annotation->span(), &base_type,
        absl::StrFormat("Expected %d parametric arguments for '%s'; got %d in "
                        "type annotation",
                        struct_def->parametric_bindings().size(),
                        struct_def->identifier(),
                        type_annotation->parametrics().size()));
  }

  absl::flat_hash_map<std::string, TypeDim> parametric_env;

  for (int64_t i = 0; i < type_annotation->parametrics().size(); ++i) {
    ParametricBinding* defined_parametric =
        struct_def->parametric_bindings()[i];
    ExprOrType eot = type_annotation->parametrics()[i];
    XLS_RET_CHECK(std::holds_alternative<Expr*>(eot));
    Expr* annotated_parametric = std::get<Expr*>(eot);
    XLS_VLOG(5) << "annotated_parametric: `" << annotated_parametric->ToString()
                << "`";

    XLS_ASSIGN_OR_RETURN(TypeDim ctd,
                         DimToConcreteUsize(annotated_parametric, ctx));
    parametric_env.emplace(defined_parametric->identifier(), std::move(ctd));
  }

  // For the remainder of the formal parameterics (i.e. after the explicitly
  // supplied ones given as arguments) we have to see if they're derived
  // parametrics. If they're *not* derived via an expression, we should have
  // been supplied some value in the annotation, so we have to flag an error!
  for (int64_t i = type_annotation->parametrics().size();
       i < struct_def->parametric_bindings().size(); ++i) {
    ParametricBinding* defined_parametric =
        struct_def->parametric_bindings()[i];
    if (defined_parametric->expr() == nullptr) {
      return TypeInferenceErrorStatus(
          type_annotation->span(), &base_type,
          absl::StrFormat("No parametric value provided for '%s' in '%s'",
                          defined_parametric->identifier(),
                          struct_def->identifier()));
    }
  }

  ParametricExpression::Env env;
  for (const auto& [k, ctd] : parametric_env) {
    if (std::holds_alternative<InterpValue>(ctd.value())) {
      env[k] = std::get<InterpValue>(ctd.value());
    } else {
      env[k] = &ctd.parametric();
    }
  }

  // Now evaluate all the dimensions according to the values we've got.
  return base_type.MapSize([&](const TypeDim& dim) -> absl::StatusOr<TypeDim> {
    if (std::holds_alternative<TypeDim::OwnedParametric>(dim.value())) {
      auto& parametric = std::get<TypeDim::OwnedParametric>(dim.value());
      return TypeDim(parametric->Evaluate(env));
    }
    return dim;
  });
}

absl::StatusOr<std::unique_ptr<Type>> DeduceTypeRefTypeAnnotation(
    const TypeRefTypeAnnotation* node, DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> base_type,
                       ctx->Deduce(node->type_ref()));
  TypeRef* type_ref = node->type_ref();
  TypeDefinition type_definition = type_ref->type_definition();

  // If it's a (potentially parametric) struct, we concretize it.
  absl::StatusOr<StructDef*> struct_def_or = DerefToStruct(
      node->span(), type_ref->ToString(), type_definition, ctx->type_info());
  if (struct_def_or.ok()) {
    auto* struct_def = struct_def_or.value();
    if (struct_def->IsParametric() && !node->parametrics().empty()) {
      XLS_ASSIGN_OR_RETURN(base_type, ConcretizeStructAnnotation(
                                          node, struct_def, *base_type, ctx));
    }
  }
  XLS_RET_CHECK(base_type->IsMeta());
  return std::move(base_type);
}

absl::StatusOr<std::unique_ptr<Type>> DeduceMatchArm(const MatchArm* node,
                                                     DeduceCtx* ctx) {
  return ctx->Deduce(node->expr());
}

absl::StatusOr<std::unique_ptr<Type>> DeduceChannelDecl(const ChannelDecl* node,
                                                        DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> element_type,
                       Deduce(node->type(), ctx));
  XLS_ASSIGN_OR_RETURN(
      element_type,
      UnwrapMetaType(std::move(element_type), node->type()->span(),
                     "channel declaration type"));
  std::unique_ptr<Type> producer = std::make_unique<ChannelType>(
      element_type->CloneToUnique(), ChannelDirection::kOut);
  std::unique_ptr<Type> consumer = std::make_unique<ChannelType>(
      std::move(element_type), ChannelDirection::kIn);

  if (node->dims().has_value()) {
    std::vector<Expr*> dims = node->dims().value();

    for (const auto& dim : dims) {
      XLS_ASSIGN_OR_RETURN(TypeDim concrete_dim, DimToConcreteUsize(dim, ctx));
      producer = std::make_unique<ArrayType>(std::move(producer), concrete_dim);
      consumer = std::make_unique<ArrayType>(std::move(consumer), concrete_dim);
    }
  }

  if (node->fifo_depth().has_value()) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> fifo_depth,
                         ctx->Deduce(node->fifo_depth().value()));
    auto want = BitsType::MakeU32();
    if (*fifo_depth != *want) {
      return ctx->TypeMismatchError(
          node->span(), node->fifo_depth().value(), *fifo_depth, nullptr, *want,
          "Channel declaration FIFO depth must be a u32.");
    }
  }

  std::vector<std::unique_ptr<Type>> elements;
  elements.push_back(std::move(producer));
  elements.push_back(std::move(consumer));
  return std::make_unique<TupleType>(std::move(elements));
}

absl::StatusOr<std::unique_ptr<Type>> DeduceRange(const Range* node,
                                                  DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> start_type,
                       ctx->Deduce(node->start()));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> end_type,
                       ctx->Deduce(node->end()));
  if (*start_type != *end_type) {
    return ctx->TypeMismatchError(node->span(), nullptr, *start_type, nullptr,
                                  *end_type,
                                  "Range start and end types didn't match.");
  }

  if (dynamic_cast<BitsType*>(start_type.get()) == nullptr) {
    return TypeInferenceErrorStatus(
        node->span(), start_type.get(),
        "Range start and end types must resolve to bits types.");
  }

  // Range implicitly defines a sized type, so it has to be constexpr
  // evaluatable.
  XLS_ASSIGN_OR_RETURN(
      InterpValue start_value,
      ConstexprEvaluator::EvaluateToValue(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          ctx->GetCurrentParametricEnv(), node->start(), start_type.get()));
  XLS_ASSIGN_OR_RETURN(
      InterpValue end_value,
      ConstexprEvaluator::EvaluateToValue(
          ctx->import_data(), ctx->type_info(), ctx->warnings(),
          ctx->GetCurrentParametricEnv(), node->end(), end_type.get()));

  XLS_ASSIGN_OR_RETURN(InterpValue le, end_value.Le(start_value));
  if (le.IsTrue()) {
    ctx->warnings()->Add(
        node->span(), WarningKind::kEmptyRangeLiteral,
        absl::StrFormat("`%s` from `%s` to `%s` is an empty range",
                        node->ToString(), start_value.ToString(),
                        end_value.ToString()));
  }

  InterpValue array_size = InterpValue::MakeUnit();
  XLS_ASSIGN_OR_RETURN(InterpValue start_ge_end, start_value.Ge(end_value));
  if (start_ge_end.IsTrue()) {
    array_size = InterpValue::MakeU32(0);
  } else {
    XLS_ASSIGN_OR_RETURN(array_size, end_value.Sub(start_value));
  }
  return std::make_unique<ArrayType>(std::move(start_type),
                                     TypeDim(array_size));
}

absl::StatusOr<std::unique_ptr<Type>> DeduceSpawn(const Spawn* node,
                                                  DeduceCtx* ctx) {
  const ParametricEnv caller_parametric_env =
      ctx->fn_stack().back().parametric_env();
  XLS_VLOG(5) << "Deducing type for invocation: `" << node->ToString()
              << "` caller symbolic bindings: " << caller_parametric_env;

  auto resolve_proc = [](const Instantiation* node,
                         DeduceCtx* ctx) -> absl::StatusOr<Proc*> {
    Expr* callee = node->callee();
    Proc* proc;
    if (auto* colon_ref = dynamic_cast<ColonRef*>(callee)) {
      XLS_ASSIGN_OR_RETURN(proc, ResolveColonRefToProc(colon_ref, ctx));
    } else {
      auto* name_ref = dynamic_cast<NameRef*>(callee);
      XLS_RET_CHECK(name_ref != nullptr);
      const std::string& callee_name = name_ref->identifier();
      XLS_ASSIGN_OR_RETURN(
          proc, GetMemberOrTypeInferenceError<Proc>(ctx->module(), callee_name,
                                                    name_ref->span()));
    }
    return proc;
  };

  XLS_ASSIGN_OR_RETURN(Proc * proc, resolve_proc(node, ctx));
  auto resolve_config = [proc](const Instantiation* node,
                               DeduceCtx* ctx) -> absl::StatusOr<Function*> {
    return &proc->config();
  };
  auto resolve_next = [proc](const Instantiation* node,
                             DeduceCtx* ctx) -> absl::StatusOr<Function*> {
    return &proc->next();
  };

  auto resolve_init = [proc](const Instantiation* node,
                             DeduceCtx* ctx) -> absl::StatusOr<Function*> {
    return &proc->init();
  };

  XLS_ASSIGN_OR_RETURN(
      TypeAndParametricEnv init_tab,
      DeduceInstantiation(ctx, down_cast<Invocation*>(node->next()->args()[0]),
                          {}, resolve_init, {}));

  // Gather up the type of all the (actual) arguments.
  std::vector<InstantiateArg> config_args;
  XLS_RETURN_IF_ERROR(InstantiateParametricArgs(
      node, node->callee(), node->config()->args(), ctx, &config_args));

  std::vector<InstantiateArg> next_args;
  next_args.push_back(
      InstantiateArg{std::make_unique<TokenType>(), node->span()});
  XLS_RETURN_IF_ERROR(InstantiateParametricArgs(
      node, node->callee(), node->next()->args(), ctx, &next_args));

  // For each [constexpr] arg, mark the associated Param as constexpr.
  absl::flat_hash_map<std::variant<const Param*, const ProcMember*>,
                      InterpValue>
      constexpr_env;
  size_t argc = node->config()->args().size();
  size_t paramc = proc->config().params().size();
  if (argc != paramc) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat("spawn had wrong argument count; want: %d got: %d",
                        paramc, argc));
  }
  for (int i = 0; i < node->config()->args().size(); i++) {
    XLS_ASSIGN_OR_RETURN(InterpValue value,
                         ConstexprEvaluator::EvaluateToValue(
                             ctx->import_data(), ctx->type_info(),
                             ctx->warnings(), ctx->GetCurrentParametricEnv(),
                             node->config()->args()[i], nullptr));
    constexpr_env.insert({proc->config().params()[i], value});
  }

  // TODO(rspringer): 2022-05-26: We can't currently lazily evaluate `next` args
  // in the BytecodeEmitter, since that'd lead to a circular dependency between
  // it and the ConstexprEvaluator, so we have to do it eagerly here.
  // Un-wind that, if possible.
  XLS_RETURN_IF_ERROR(ConstexprEvaluator::Evaluate(
      ctx->import_data(), ctx->type_info(), ctx->warnings(),
      ctx->GetCurrentParametricEnv(),
      down_cast<Invocation*>(node->next()->args()[0]),
      /*type=*/nullptr));

  XLS_ASSIGN_OR_RETURN(TypeAndParametricEnv tab,
                       DeduceInstantiation(ctx, node->config(), config_args,
                                           resolve_config, constexpr_env));

  XLS_ASSIGN_OR_RETURN(TypeInfo * config_ti,
                       ctx->type_info()->GetInvocationTypeInfoOrError(
                           node->config(), caller_parametric_env));

  // Now we need to get the [constexpr] Proc member values so we can set them
  // when typechecking the `next` function. Those values are the elements in the
  // `config` function's terminating XlsTuple.
  // 1. Get the last statement in the `config` function.
  Function& config_fn = proc->config();
  Expr* last =
      std::get<Expr*>(config_fn.body()->statements().back()->wrapped());
  const XlsTuple* tuple = dynamic_cast<const XlsTuple*>(last);
  XLS_RET_CHECK_NE(tuple, nullptr);

  // 2. Extract the value of each element and associate with the corresponding
  // Proc member (in decl. order).
  constexpr_env.clear();
  XLS_RET_CHECK_EQ(tuple->members().size(), proc->members().size());
  for (int i = 0; i < tuple->members().size(); i++) {
    XLS_ASSIGN_OR_RETURN(
        InterpValue value,
        ConstexprEvaluator::EvaluateToValue(
            ctx->import_data(), config_ti, ctx->warnings(),
            ctx->GetCurrentParametricEnv(), tuple->members()[i], nullptr));
    constexpr_env.insert({proc->members()[i], value});
  }

  XLS_RETURN_IF_ERROR(DeduceInstantiation(ctx, node->next(), next_args,
                                          resolve_next, constexpr_env)
                          .status());
  return Type::MakeUnit();
}

absl::StatusOr<std::unique_ptr<Type>> DeduceNameRef(const NameRef* node,
                                                    DeduceCtx* ctx) {
  AstNode* name_def = ToAstNode(node->name_def());
  XLS_RET_CHECK(name_def != nullptr);

  std::optional<Type*> item = ctx->type_info()->GetItem(name_def);
  if (item.has_value()) {
    auto type = (*item)->CloneToUnique();
    return type;
  }

  // If this has no corresponding type because it is a parametric function that
  // is not being invoked, we give an error instead of propagating
  // "TypeMissing".
  if ((IsParametricFunction(node->GetDefiner()) ||
       IsBuiltinParametricNameRef(node)) &&
      !ParentIsInvocationWithCallee(node)) {
    return TypeInferenceErrorStatus(
        node->span(), nullptr,
        absl::StrFormat(
            "Name '%s' is a parametric function, but it is not being invoked",
            node->identifier()));
  }

  return TypeMissingErrorStatus(/*node=*/*name_def, /*user=*/node);
}

absl::StatusOr<std::unique_ptr<Type>> DeduceConstRef(const ConstRef* node,
                                                     DeduceCtx* ctx) {
  XLS_VLOG(3) << "DeduceConstRef; node: `" << node->ToString() << "` @ "
              << node->span();
  // ConstRef is a subtype of NameRef, same deduction rule works.
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type, DeduceNameRef(node, ctx));
  XLS_ASSIGN_OR_RETURN(InterpValue value,
                       ctx->type_info()->GetConstExpr(node->name_def()));
  XLS_VLOG(3) << " DeduceConstRef; value: " << value.ToString();
  ctx->type_info()->NoteConstExpr(node, std::move(value));
  return type;
}

class DeduceVisitor : public AstNodeVisitor {
 public:
  explicit DeduceVisitor(DeduceCtx* ctx) : ctx_(ctx) {}

#define DEDUCE_DISPATCH(__type, __rule)                   \
  absl::Status Handle##__type(const __type* n) override { \
    result_ = __rule(n, ctx_);                            \
    return result_.status();                              \
  }

  DEDUCE_DISPATCH(Unop, DeduceUnop)
  DEDUCE_DISPATCH(Param, DeduceParam)
  DEDUCE_DISPATCH(ProcMember, DeduceProcMember)
  DEDUCE_DISPATCH(ConstantDef, DeduceConstantDef)
  DEDUCE_DISPATCH(Number, DeduceNumber)
  DEDUCE_DISPATCH(String, DeduceString)
  DEDUCE_DISPATCH(TypeRef, DeduceTypeRef)
  DEDUCE_DISPATCH(TypeAlias, DeduceTypeAlias)
  DEDUCE_DISPATCH(XlsTuple, DeduceXlsTuple)
  DEDUCE_DISPATCH(Conditional, DeduceConditional)
  DEDUCE_DISPATCH(Binop, DeduceBinop)
  DEDUCE_DISPATCH(EnumDef, DeduceEnumDef)
  DEDUCE_DISPATCH(Let, DeduceLet)
  DEDUCE_DISPATCH(For, DeduceFor)
  DEDUCE_DISPATCH(Cast, DeduceCast)
  DEDUCE_DISPATCH(ConstAssert, DeduceConstAssert)
  DEDUCE_DISPATCH(StructDef, DeduceStructDef)
  DEDUCE_DISPATCH(Array, DeduceArray)
  DEDUCE_DISPATCH(Attr, DeduceAttr)
  DEDUCE_DISPATCH(Block, DeduceBlock)
  DEDUCE_DISPATCH(ChannelDecl, DeduceChannelDecl)
  DEDUCE_DISPATCH(ConstantArray, DeduceConstantArray)
  DEDUCE_DISPATCH(ColonRef, DeduceColonRef)
  DEDUCE_DISPATCH(Index, DeduceIndex)
  DEDUCE_DISPATCH(Match, DeduceMatch)
  DEDUCE_DISPATCH(Range, DeduceRange)
  DEDUCE_DISPATCH(Spawn, DeduceSpawn)
  DEDUCE_DISPATCH(SplatStructInstance, DeduceSplatStructInstance)
  DEDUCE_DISPATCH(Statement, DeduceStatement)
  DEDUCE_DISPATCH(StructInstance, DeduceStructInstance)
  DEDUCE_DISPATCH(TupleIndex, DeduceTupleIndex)
  DEDUCE_DISPATCH(UnrollFor, DeduceUnrollFor)
  DEDUCE_DISPATCH(BuiltinTypeAnnotation, DeduceBuiltinTypeAnnotation)
  DEDUCE_DISPATCH(ChannelTypeAnnotation, DeduceChannelTypeAnnotation)
  DEDUCE_DISPATCH(ArrayTypeAnnotation, DeduceArrayTypeAnnotation)
  DEDUCE_DISPATCH(TupleTypeAnnotation, DeduceTupleTypeAnnotation)
  DEDUCE_DISPATCH(TypeRefTypeAnnotation, DeduceTypeRefTypeAnnotation)
  DEDUCE_DISPATCH(MatchArm, DeduceMatchArm)
  DEDUCE_DISPATCH(Invocation, DeduceInvocation)
  DEDUCE_DISPATCH(FormatMacro, DeduceFormatMacro)
  DEDUCE_DISPATCH(ZeroMacro, DeduceZeroMacro)
  DEDUCE_DISPATCH(ConstRef, DeduceConstRef)
  DEDUCE_DISPATCH(NameRef, DeduceNameRef)

  // Unhandled nodes for deduction, either they are custom visited or not
  // visited "automatically" in the traversal process (e.g. top level module
  // members).
  absl::Status HandleProc(const Proc* n) override { return Fatal(n); }
  absl::Status HandleSlice(const Slice* n) override { return Fatal(n); }
  absl::Status HandleImport(const Import* n) override { return Fatal(n); }
  absl::Status HandleFunction(const Function* n) override { return Fatal(n); }
  absl::Status HandleQuickCheck(const QuickCheck* n) override {
    return Fatal(n);
  }
  absl::Status HandleTestFunction(const TestFunction* n) override {
    return Fatal(n);
  }
  absl::Status HandleTestProc(const TestProc* n) override { return Fatal(n); }
  absl::Status HandleModule(const Module* n) override { return Fatal(n); }
  absl::Status HandleWidthSlice(const WidthSlice* n) override {
    return Fatal(n);
  }
  absl::Status HandleNameDefTree(const NameDefTree* n) override {
    return Fatal(n);
  }
  absl::Status HandleNameDef(const NameDef* n) override { return Fatal(n); }
  absl::Status HandleBuiltinNameDef(const BuiltinNameDef* n) override {
    return Fatal(n);
  }
  absl::Status HandleParametricBinding(const ParametricBinding* n) override {
    return Fatal(n);
  }
  absl::Status HandleWildcardPattern(const WildcardPattern* n) override {
    return Fatal(n);
  }

  absl::StatusOr<std::unique_ptr<Type>>& result() { return result_; }

 private:
  absl::Status Fatal(const AstNode* n) {
    XLS_LOG(FATAL) << "Got unhandled AST node for deduction: " << n->ToString();
  }

  DeduceCtx* ctx_;
  absl::StatusOr<std::unique_ptr<Type>> result_;
};

absl::StatusOr<std::unique_ptr<Type>> DeduceInternal(const AstNode* node,
                                                     DeduceCtx* ctx) {
  DeduceVisitor visitor(ctx);
  XLS_RETURN_IF_ERROR(node->Accept(&visitor));
  return std::move(visitor.result());
}

absl::StatusOr<std::unique_ptr<Type>> ResolveViaEnv(
    const Type& type, const ParametricEnv& parametric_env) {
  ParametricExpression::Env env;
  for (const auto& [k, v] : parametric_env.bindings()) {
    env[k] = v;
  }

  return type.MapSize([&](const TypeDim& dim) -> absl::StatusOr<TypeDim> {
    if (std::holds_alternative<TypeDim::OwnedParametric>(dim.value())) {
      const auto& parametric = std::get<TypeDim::OwnedParametric>(dim.value());
      return TypeDim(parametric->Evaluate(env));
    }
    return dim;
  });
}

}  // namespace

absl::StatusOr<std::unique_ptr<Type>> Resolve(const Type& type,
                                              DeduceCtx* ctx) {
  XLS_RET_CHECK(!ctx->fn_stack().empty());
  const FnStackEntry& entry = ctx->fn_stack().back();
  const ParametricEnv& fn_parametric_env = entry.parametric_env();
  return ResolveViaEnv(type, fn_parametric_env);
}

absl::StatusOr<std::unique_ptr<Type>> Deduce(const AstNode* node,
                                             DeduceCtx* ctx) {
  XLS_RET_CHECK(node != nullptr);
  if (std::optional<Type*> type = ctx->type_info()->GetItem(node)) {
    return (*type)->CloneToUnique();
  }
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> type, DeduceInternal(node, ctx));
  XLS_RET_CHECK(type != nullptr);
  ctx->type_info()->SetItem(node, *type);
  XLS_VLOG(5) << absl::StreamFormat(
      "Deduced type of `%s` @ %p (kind: %s) => %s in %p", node->ToString(),
      node, node->GetNodeTypeName(), type->ToString(), ctx->type_info());

  return type;
}

absl::StatusOr<std::unique_ptr<Type>> DeduceAndResolve(const AstNode* node,
                                                       DeduceCtx* ctx) {
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> deduced, ctx->Deduce(node));
  return Resolve(*deduced, ctx);
}

// Converts a sequence of ParametricBinding AST nodes into a sequence of
// ParametricConstraints (which decorate the ParametricBinding nodes with their
// deduced Types).
absl::StatusOr<std::vector<ParametricConstraint>>
ParametricBindingsToConstraints(absl::Span<ParametricBinding* const> bindings,
                                DeduceCtx* ctx) {
  std::vector<ParametricConstraint> parametric_constraints;
  for (ParametricBinding* binding : bindings) {
    XLS_ASSIGN_OR_RETURN(std::unique_ptr<Type> binding_type,
                         ParametricBindingToType(binding, ctx));
    parametric_constraints.push_back(
        ParametricConstraint(*binding, std::move(binding_type)));
  }
  return parametric_constraints;
}

}  // namespace xls::dslx
