/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/tvm/relax/ir/dataflow_matcher.cc
 * \brief The dataflow pattern matcher for Relax.
 */

#include <tvm/relax/dataflow_matcher.h>
#include <tvm/relax/dataflow_pattern.h>
#include <tvm/relax/expr.h>
#include <tvm/relax/expr_functor.h>
#include <tvm/tir/op.h>

#include <stack>

#include "dataflow_matcher_impl.h"

namespace tvm {
namespace relax {

using tvm::arith::Analyzer;

// Pattern Matcher
bool DFPatternMatcher::Match(const DFPattern& pattern, const Expr& expr, bool autojump) {
  autojump_ = autojump;
  memo_.clear();
  matched_nodes_.clear();
  return VisitDFPattern(pattern, expr);
}

static Expr TryGetValOfVar(const Expr& expr, const runtime::Map<Var, Expr>& var2val,
                           bool autojump) {
  // `autojump` means when meeting a relax.Var, we automatically jump to
  // match its corresponding expression instead.
  if (!autojump) return expr;

  // if not match, try to match value of var if expr is a var.
  if (const VarNode* var = expr.as<VarNode>()) {
    ICHECK(var2val.defined()) << "The relax.Var->Expr mapping should be given to perform autojump.";
    auto may = var2val.Get(GetRef<Var>(var));
    if (may.defined()) return may.value();
  }

  return expr;
}

void DFPatternMatcher::ClearMap(size_t watermark) {
  for (size_t i = watermark; i < matched_nodes_.size(); ++i) {
    memo_.erase(matched_nodes_[i]);
  }
  matched_nodes_.erase(matched_nodes_.begin() + watermark, matched_nodes_.end());
}

bool DFPatternMatcher::VisitDFPattern(const DFPattern& pattern, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  if (memoize_ && memo_.count(pattern)) {
    ICHECK_EQ(memo_[pattern].size(), 1);
    return expr.same_as(memo_[pattern][0]);
  } else {
    auto watermark = matched_nodes_.size();
    auto out = DFPatternFunctor::VisitDFPattern(pattern, expr);
    if (out) {
      memo_[pattern].push_back(expr);
      matched_nodes_.push_back(pattern);
    } else {
      ClearMap(watermark);
    }
    return out;
  }
}

bool DFPatternMatcher::VisitDFPattern_(const OrPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  return VisitDFPattern(op->left, expr) || VisitDFPattern(op->right, expr);
}

bool DFPatternMatcher::VisitDFPattern_(const AndPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  return VisitDFPattern(op->left, expr) && VisitDFPattern(op->right, expr);
}

bool DFPatternMatcher::VisitDFPattern_(const NotPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  return !VisitDFPattern(op->reject, expr);
}

bool MatchRetValue(const ObjectRef& lhs, const TVMRetValue& rhs) {
  switch (rhs.type_code()) {
    case kDLInt:
      if (auto* val = lhs.as<IntImmNode>()) {
        return val->value == rhs.operator int64_t();
      }
      break;
    case kDLFloat:
      if (auto* val = lhs.as<FloatImmNode>()) {
        return val->value == rhs.operator double();
      }
      break;
    case kTVMStr:
      if (auto* val = lhs.as<tir::StringImmNode>()) {
        return val->value == rhs.operator std::string();
      } else if (auto* val = lhs.as<StringObj>()) {
        return val->data == rhs.operator std::string();
      }
      break;
    case kTVMDataType:
      if (auto* val = lhs.as<tir::StringImmNode>()) {
        return rhs.operator std::string() == val->value;
      } else if (auto* val = lhs.as<StringObj>()) {
        return rhs.operator std::string() == val->data;
      } else {
        ICHECK(false) << "PatternMatcher: Unsupported TVMDataType " << lhs;
      }
      break;
    case kTVMObjectHandle:
      if (rhs.IsObjectRef<String>()) {
        if (auto* val = lhs.as<tir::StringImmNode>()) {
          return rhs.operator String() == val->value;
        } else if (auto* val = lhs.as<StringObj>()) {
          return rhs.operator String() == val->data;
        }
      } else {
        // Compare the objects for structural equality
        static auto* structural_equal = runtime::Registry::Get("node.StructuralEqual");
        ICHECK(structural_equal) << "node.StructuralEqual is not registered.";
        if ((*structural_equal)(lhs, GetRef<ObjectRef>(rhs.ptr<Object>()), false, true)) {
          return true;
        }
      }
      break;
    default:
      ICHECK(false) << "Unsupported type code in Pattern Node " << rhs.type_code();
  }
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const AttrPatternNode* attr_pattern, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  bool matches = VisitDFPattern(attr_pattern->pattern, expr);
  if (!matches) return matches;
  VLOG(1) << "considering AttrPatternNode at:\n" << PrettyPrint(expr);
  auto attributes = attr_pattern->attrs.as<DictAttrsNode>()->dict;
  if (const auto* op_node = expr.as<OpNode>()) {
    Op op = GetRef<Op>(op_node);
    for (auto kv : attributes) {
      auto attr_name = kv.first;
      auto attr_value = kv.second;
      if (Op::HasAttrMap(attr_name)) {
        auto op_map = Op::GetAttrMap<TVMRetValue>(attr_name);
        if (op_map.count(op)) {
          matches &= MatchRetValue(attr_value, op_map[op]);
        } else {
          matches = false;
        }
      } else {
        matches = false;
      }
    }
  } else if (auto* op = expr.as<CallNode>()) {
    matches = true;
    // TODO(mbrookhart): When OpNode Attrs move from TVMRetValue to the Object system, remove this
    // and replace the whole thing with a Visitor-based approach
    ReflectionVTable* reflection = ReflectionVTable::Global();
    auto attrs_node = const_cast<BaseAttrsNode*>(op->attrs.get());
    // attrs may be undefined on non-op calls so we check first
    std::vector<std::string> attr_names;
    if (attrs_node) {
      attr_names = reflection->ListAttrNames(attrs_node);
    }
    for (auto kv : attributes) {
      std::string attr = kv.first;
      if (matches && std::find(attr_names.begin(), attr_names.end(), attr) != attr_names.end()) {
        matches &= MatchRetValue(kv.second, reflection->GetAttr(attrs_node, attr));
      } else {
        matches = false;
        break;
      }
    }
  } else if (auto* op = expr.as<FunctionNode>()) {
    matches = true;
    for (auto kv : attributes) {
      if (matches && op->attrs.defined() && op->attrs->dict.count(kv.first)) {
        matches &= StructuralEqual()(kv.second, op->attrs->dict[kv.first]);
      } else {
        matches = false;
        break;
      }
    }
  } else {
    matches = false;
  }
  return matches;
}

Array<DFPattern> reverse(const Array<DFPattern>& args) {
  Array<DFPattern> new_args;
  for (auto it = args.rbegin(); it != args.rend(); ++it) {
    new_args.push_back(*it);
  }
  return new_args;
}

bool DFPatternMatcher::VisitDFPattern_(const CallPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  // utilities
  auto get_op_node = [](const CallPatternNode* op) -> const tvm::OpNode* {
    if (op) {
      if (auto* expr_pattern = op->op.as<ExprPatternNode>()) {
        return expr_pattern->expr.as<OpNode>();
      }
    }
    return nullptr;
  };
  auto is_pattern_op = [&get_op_node](const CallPatternNode* op, std::string op_type) {
    if (const auto* op_node = get_op_node(op)) {
      if (op_node->name == op_type) {
        return true;
      }
    }
    return false;
  };
  auto is_expr_op = [](const Expr& expr, std::string op_type) {
    if (const auto* call_node = expr.as<CallNode>()) {
      if (const auto* op_node = call_node->op.as<OpNode>()) {
        if (op_node->name == op_type) {
          return true;
        }
      }
    }
    return false;
  };

  // logic
  auto watermark = matched_nodes_.size();
  if (const auto* call_node = expr.as<CallNode>()) {
    auto matches_op = VisitDFPattern(op->op, call_node->op);
    if (matches_op) {
      auto watermark2 = matched_nodes_.size();

      auto match_args = [this, &watermark2](const Array<DFPattern> pattern_args,
                                            const Array<Expr> expr_args) {
        bool matches = true;
        size_t i = 0;
        if (pattern_args.defined()) {
          if (pattern_args.size() == expr_args.size()) {
            while (matches && i < pattern_args.size()) {
              matches &= VisitDFPattern(pattern_args[i], expr_args[i]);
              ++i;
            }
          } else {
            matches = false;
          }
        }
        if (!matches) {
          ClearMap(watermark2);
        }
        return matches;
      };

      // Standard case
      if (match_args(op->args, call_node->args)) {
        return true;
      }
      // Commutative Matching
      if (const OpNode* op_node = get_op_node(op)) {
        if ((op_node->name == "add") || (op_node->name == "multiply")) {
          if (match_args(reverse(op->args), call_node->args)) {
            return true;
          }
        }
      }
    } else {
      ClearMap(watermark);
      // associate divide/multiply
      if (is_pattern_op(op, "divide")) {
        if (const auto* arg_node = op->args[0].as<CallPatternNode>()) {
          if (is_pattern_op(arg_node, "multiply") && is_expr_op(expr, "multiply") &&
              (is_expr_op(call_node->args[0], "divide") ||
               is_expr_op(call_node->args[1], "divide"))) {
            bool out = false;
            for (size_t arg_id = 0; arg_id < 2; ++arg_id) {
              auto div = CallPattern(op->op, {arg_node->args[arg_id], op->args[1]});
              auto mul = CallPattern(arg_node->op, {arg_node->args[(arg_id + 1) % 2], div});
              out = VisitDFPattern(mul, expr);
              if (out) {
                return true;
              } else {
                ClearMap(watermark);
              }
            }
            return out;
          }
        }
      }
      if (is_pattern_op(op, "multiply")) {
        // associate multiply/divide
        for (size_t arg_id = 0; arg_id < 2; ++arg_id) {
          if (auto* arg_node = op->args[arg_id].as<CallPatternNode>()) {
            if (is_pattern_op(arg_node, "divide") && is_expr_op(expr, "divide") &&
                (is_expr_op(call_node->args[0], "multiply") ||
                 is_expr_op(call_node->args[1], "multiply"))) {
              auto mul = CallPattern(op->op, {arg_node->args[0], op->args[(arg_id + 1) % 2]});
              auto div = CallPattern(arg_node->op, {mul, arg_node->args[1]});
              return VisitDFPattern(div, expr);
            }
          }
        }
      }
    }
  }
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const ExprPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  return StructuralEqual()(op->expr, expr);
}

bool DFPatternMatcher::VisitDFPattern_(const FunctionPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  bool matches = false;
  if (const auto* func = expr.as<FunctionNode>()) {
    matches = true;
    if (op->params.defined()) {
      size_t i = 0;
      if (op->params.size() == func->params.size()) {
        while (matches && i < op->params.size()) {
          matches &= VisitDFPattern(op->params[i], func->params[i]);
          ++i;
        }
      } else {
        matches = false;
      }
    }
    if (matches) {
      matches &= VisitDFPattern(op->body, func->body);
    }
  }
  return matches;
}

bool DFPatternMatcher::VisitDFPattern_(const TupleGetItemPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  bool matches = false;
  if (const auto* tuple_get_item_node = expr.as<TupleGetItemNode>()) {
    matches = (op->index == -1 || op->index == tuple_get_item_node->index) &&
              VisitDFPattern(op->tuple, tuple_get_item_node->tuple);
  }
  return matches;
}

bool DFPatternMatcher::VisitDFPattern_(const TuplePatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  bool matches = false;
  if (const auto* tuple_node = expr.as<TupleNode>()) {
    matches = true;
    if (op->fields.defined()) {
      if (op->fields.size() == tuple_node->fields.size()) {
        size_t i = 0;
        while (matches && i < op->fields.size()) {
          matches &= VisitDFPattern(op->fields[i], tuple_node->fields[i]);
          ++i;
        }
      } else {
        matches = false;
      }
    }
  }
  return matches;
}

bool DFPatternMatcher::VisitDFPattern_(const TypePatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  auto expr_type = expr.as<ExprNode>()->checked_type();
  return (StructuralEqual()(op->type, expr_type)) && VisitDFPattern(op->pattern, expr);
}

auto shape_equal = [](Analyzer& analyzer, const Array<PrimExpr>& lhs, const Array<PrimExpr>& rhs) {
  if (lhs.size() != rhs.size()) return false;
  for (size_t i = 0; i < lhs.size(); ++i)
    if (!tir::is_one(analyzer.Simplify(lhs[i] == rhs[i]))) return false;
  return true;
};

bool DFPatternMatcher::VisitDFPattern_(const ShapePatternNode* op, const Expr& expr) {
  // no need to jump, as var.shape == value.shape
  if (const ShapeExprNode* shape_expr = expr->shape().as<ShapeExprNode>())
    return shape_equal(analyzer_, op->shape, shape_expr->values) &&
           VisitDFPattern(op->pattern, expr);
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const PrimArrPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  if (const ShapeExprNode* shape_expr = expr.as<ShapeExprNode>())
    return shape_equal(analyzer_, op->array, shape_expr->values);
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const DataTypePatternNode* op, const Expr& expr) {
  // no need to jump, as var.dtype == value.dtype
  auto expr_type = expr.as<ExprNode>()->checked_type();
  if (const DynTensorTypeNode* tensor_type = expr_type.as<DynTensorTypeNode>()) {
    return (StructuralEqual()(op->dtype, tensor_type->dtype)) && VisitDFPattern(op->pattern, expr);
  }
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const VarPatternNode* op, const Expr& expr) {
  // We don't jump for var pattern, as there's no need to access its value to judge it.
  if (const auto* var_node = expr.as<VarNode>()) {
    // "" means any name.
    return "" == op->name_hint() || op->name_hint() == var_node->name_hint();
  }
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const ExternFuncPatternNode* op, const Expr& expr0) {
  auto expr = TryGetValOfVar(expr0, var2val_, autojump_);
  if (const auto* extern_fn = expr.as<ExternFuncNode>()) {
    return "" == op->global_symbol() || op->global_symbol() == extern_fn->global_symbol;
  }
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const ConstantPatternNode* op, const Expr& expr) {
  // No need to bother var's value when checking constants.
  return expr.as<ConstantNode>() != nullptr;
}

bool DFPatternMatcher::VisitDFPattern_(const DataflowVarPatternNode* op, const Expr& expr) {
  // DataflowVar is inherented from Var, so dispatch it to VarPattern.
  return expr->IsInstance<DataflowVarNode>() &&
         VisitDFPattern_(static_cast<const VarPatternNode*>(op), expr);
}

bool DFPatternMatcher::VisitDFPattern_(const GlobalVarPatternNode* op, const Expr& expr) {
  // GlobalVarPattern is not inherited from Var, so we need to handle it separately.
  if (const auto* var_node = expr.as<GlobalVarNode>())
    return "" == op->name_hint() || op->name_hint() == var_node->name_hint;
  return false;
}

bool DFPatternMatcher::VisitDFPattern_(const WildcardPatternNode* op, const Expr& expr) {
  return true;
}

bool DFPatternMatcher::VisitDFPattern_(const RuntimeDepShapePatternNode* op, const Expr& expr) {
  return expr->shape_->IsInstance<RuntimeDepShapeNode>();
}

bool MatchExprPattern(DFPattern pattern, Expr expr, Optional<runtime::Map<Var, Expr>> var2val) {
  if (var2val.defined())  // autojump is enabled with var2val.
    return DFPatternMatcher(std::move(var2val.value())).Match(pattern, expr, true);
  else
    return DFPatternMatcher().Match(pattern, expr, false);
}

TVM_REGISTER_GLOBAL("relax.dataflow_pattern.match_expr").set_body_typed(MatchExprPattern);

}  // namespace relax
}  // namespace tvm
