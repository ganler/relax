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
 * \file tvm/relax/stmt_rewrite.h
 * \brief An IR rewriter to easily rewrites statements.
 */

#ifndef TVM_RELAX_IR_STMT_REWRITE_H_

#include <tvm/relax/analysis.h>
#include <tvm/relax/expr.h>

#include <type_traits>

namespace tvm {
namespace relax {

/*! \brief Statement rewriter for relax.DataflowBlock. */
class DataflowBlockRewriteNode : public Object {
 public:
  /*! \brief Replace all uses of old_var with new_var. */
  void ReplaceAllUses(Var old_var, Var new_var);
  /*! \brief Insert a Binding statement. */
  void Add(Binding binding);
  /*! \brief Insert an expression as VarBinding with variable name. */
  void Add(String var_name, Expr expr, bool is_dfvar = false) {
    auto var = is_dfvar ? DataflowVar(var_name, expr->shape(), expr->checked_type())
                        : Var(var_name, expr->shape(), expr->checked_type());
    Add(VarBinding(std::move(var), std::move(expr)));
  }
  /*! \brief Insert an expression as VarBinding with automatic variable name. */
  void Add(Expr expr, bool is_dfvar = false) { Add(make_new_varname(), expr, is_dfvar); }
  /*! \brief Remove the definition statement of an unused variable. */
  void RemoveUnused(Var unused);

  /*! \brief The rewritten dataflow block. */
  DataflowBlock MutatedDataflowBlock() { return dfb_.object; }
  /*! \brief The rewritten function. */
  Function MutatedFunc() { return root_fn_.object; }
  /*! \brief The rewritten IRModule. */
  IRModule MutateIRModule(IRModule irmod);

  /*! \brief Visit attributes. */
  void VisitAttrs(AttrVisitor* v) {
    v->Visit("dfb", &dfb_.object);
    v->Visit("root_fn", &root_fn_.object);
  }

  static constexpr const char* _type_key = "relax.DataflowBlockRewrite";
  TVM_DECLARE_FINAL_OBJECT_INFO(DataflowBlockRewriteNode, Object);

 protected:
  friend class DataflowBlockRewrite;

  /*! \brief Mutable reference with reference counting. */
  template <typename T, std::enable_if_t<std::is_base_of<ObjectRef, T>::value, int> = 0>
  struct RefCntPtr {
    T object;

    RefCntPtr() : object(nullptr) {}
    RefCntPtr(T v) : object(v) {}

    T operator->() const { return object; }
    operator T() const { return object; }
    auto get() const { return object.get(); }
  };

  RefCntPtr<DataflowBlock> dfb_;            //!< The rewritten dataflow block.
  RefCntPtr<Function> root_fn_;             //!< The rewritten function.
  const FunctionNode* original_fn_ptr_;     //!< Pointer to the original function.
  runtime::Map<Var, Array<Var>> to_users_;  //!< Map from variable to its users.

 private:
  /*! \brief Generate a new variable name. */
  String make_new_varname();

  std::set<String> used_names_ = {};  //!< Set of used variable names.
  size_t counter_ = 0;                //!< Counter for generating new variable names.
};

/*!
 * \brief A statement rewriter for relax.DataflowBlock.
 * \sa DataflowBlockRewriteNode
 */
class DataflowBlockRewrite : public ObjectRef {
 public:
  TVM_DLL explicit DataflowBlockRewrite(DataflowBlock dfb, Function root_fn);

  /*!
   * \brief mutable accessor.
   * \return mutable access pointer.
   */
  DataflowBlockRewriteNode* operator->() {
    ICHECK(get() != nullptr);
    return static_cast<DataflowBlockRewriteNode*>(get_mutable());
  }

  TVM_DEFINE_OBJECT_REF_METHODS(DataflowBlockRewrite, ObjectRef, DataflowBlockRewriteNode);
};

}  // namespace relax
}  // namespace tvm

#define TVM_RELAX_IR_STMT_REWRITE_H_
#endif  // TVM_RELAX_IR_STMT_REWRITE_H_
