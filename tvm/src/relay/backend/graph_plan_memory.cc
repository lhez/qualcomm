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
 * \file relay/backend/graph_plan_memory.cc
 * \brief Memory index assignment pass for executing
 *   the program in the graph runtime.
 */
#include <tvm/relay/analysis.h>
#include <tvm/relay/expr.h>
#include <tvm/relay/expr_functor.h>
#include <tvm/tir/op.h>
#include <tvm/target/target.h>

#include "../../support/arena.h"
#include "../../runtime/texture.h"

namespace tvm {
namespace relay {

using TargetsMap = Map<Integer, Target>;
using Texture2DShape = runtime::Texture2DShape<int64_t>;
constexpr auto Is2DStorage = runtime::IsTextureStorage;

struct StorageToken {
  /*! \brief Reference counter */
  int ref_counter{0};
  /*! \brief number of bytes */
  size_t max_bytes{0};
  /*! \brief The corresponding tensor type node. */
  const TensorTypeNode* ttype{nullptr};
  /*! \brief virtual device index that corresponds to the device_type in
   * DLContext. */
  int device_type{0};
  /*! \brief The storage id */
  int64_t storage_id{-1};
  /*! \brief The storage scope */
  std::string storage_scope{"global"};
};

class StorageAllocaBaseVisitor : public ExprVisitor {
 public:
  // run the visitor on a function.
  void Run(const Function& func) {
    for (Var param : func->params) {
      CreateToken(param.operator->(), false);
    }
    // must always keep output alive.
    for (StorageToken* tok : GetToken(func->body)) {
      tok->ref_counter += 1;
    }
  }

  void VisitExpr_(const ConstantNode* op) final { this->CreateToken(op, false); }

  void VisitExpr_(const VarNode* op) final {
    // Do nothing.
  }

  void VisitExpr_(const FunctionNode* op) final {
    // do not recurse into sub function.
  }

  void VisitExpr_(const GlobalVarNode* op) final {
    // Do nothing.
  }

  void VisitExpr_(const OpNode* op) final {
    // Do nothing.
  }

  void VisitExpr_(const TupleNode* op) final {
    std::vector<StorageToken*> fields;
    for (Expr field : op->fields) {
      auto tokens = GetToken(field);
      fields.insert(fields.end(), tokens.begin(), tokens.end());
    }
    token_map_[op] = fields;
  }

  void VisitExpr_(const TupleGetItemNode* op) final {
    const auto& tok = GetToken(op->tuple);
    ICHECK_LT(static_cast<size_t>(op->index), tok.size());
    token_map_[op] = {tok[op->index]};
  }

  void VisitExpr_(const IfNode* op) final { LOG(FATAL) << "if is not supported."; }

  void VisitExpr_(const LetNode* op) final {
    auto token = GetToken(op->value);
    token_map_[op->var.operator->()] = token;
    token_map_[op] = GetToken(op->body);
  }

 protected:
  /*! \brief internal token map */
  std::unordered_map<const ExprNode*, std::vector<StorageToken*> > token_map_;

  /*!
   * \brief Get the necessary token.
   * \param expr The expression.
   * \return The corresponding token.
   */
  const std::vector<StorageToken*>& GetToken(const Expr& expr) {
    this->VisitExpr(expr);
    auto it = token_map_.find(expr.operator->());
    ICHECK(it != token_map_.end());
    return it->second;
  }
  /*!
   * \brief Populate the token map to set op's tokens
   * \param op The node to be processed.
   * \param can_realloc Whether we can re-allocate the memory.
   */
  virtual void CreateToken(const ExprNode* op, bool can_realloc) = 0;
};

/*!
 * \brief Collect the target specific tensor storage info for each expression's output.
 * \param expr The expression.
 * \param expr The device id map which can be used to infer device specific storage scope availability.
 * \param expr The target mapping from device id to target.
 * \return The device based storage mapping.
 */
Map<Expr, Array<String>> CollectStorageInfo(const Expr& expr, const Map<Expr, Integer>& dev_map, const TargetsMap& target_map) {
  auto less = [](Integer i, Integer j) {
    auto i_imm = i.as<tir::IntImmNode>();
    auto j_imm = j.as<tir::IntImmNode>();
    ICHECK(i_imm && j_imm);
    return i_imm->value < j_imm->value;
  };
  std::set<Integer, decltype(less)> device_types(less);
  for (auto& kv : target_map) {
    device_types.insert(kv.first);
  }
  std::string ftarget_prefix = "relay.backend";
  for (auto& dev_id : device_types) {
    Target target = target_map[dev_id];
    ftarget_prefix += ("." + target->kind->name);
    if (Optional<String> t_device = target->GetAttr<String>("device")) {
      ftarget_prefix += ("." + t_device.value());
    }
  }
  Map<Expr, Array<String>> storage_info = {};
  if (const auto* f = runtime::Registry::Get(ftarget_prefix + "._CollectStorageInfo")) {
    storage_info = (*f)(expr, dev_map, target_map);
  }
  return storage_info;
}

class StorageAllocaInit : protected StorageAllocaBaseVisitor {
 public:
  explicit StorageAllocaInit(support::Arena* arena) : arena_(arena) {}

  /*! \return The internal token map */
  std::unordered_map<const ExprNode*, std::vector<StorageToken*> > GetInitTokenMap(
      const Function& func, const TargetsMap& targets) {
    node_device_map_ = CollectDeviceInfo(func);
    node_storage_map_ = CollectStorageInfo(func, node_device_map_, targets);
    this->Run(func);
    return std::move(token_map_);
  }

 protected:
  using StorageAllocaBaseVisitor::VisitExpr_;

  void CreateToken(const ExprNode* op, bool can_realloc) final {
    ICHECK(!token_map_.count(op));
    std::vector<StorageToken*> tokens;
    auto expr = GetRef<Expr>(op);
    int device_type =
      node_device_map_.count(expr) ? node_device_map_[expr]->value : 0;

    Optional<Array<String>> storage_info;
    if (node_storage_map_.count(GetRef<Expr>(op))) {
      storage_info = node_storage_map_[GetRef<Expr>(op)];
    }

    if (const auto* tuple_type = op->checked_type().as<TupleTypeNode>()) {
      if (storage_info.defined()) { ICHECK_EQ(tuple_type->fields.size(), storage_info.value().size()); }
      for (size_t i = 0; i < tuple_type->fields.size(); i++) {
        const auto* ttype = tuple_type->fields[i].as<TensorTypeNode>();
        ICHECK(ttype);
        StorageToken* token = arena_->make<StorageToken>();
        token->ttype = ttype;
        token->device_type = device_type;
        if (storage_info.defined()) {
          token->storage_scope = storage_info.value()[i];
        }
        tokens.push_back(token);
      }
    } else {
      const auto* ttype = op->checked_type().as<TensorTypeNode>();
      ICHECK(ttype);
      StorageToken* token = arena_->make<StorageToken>();
      token->ttype = ttype;
      token->device_type = device_type;
      if (storage_info.defined()) {
        token->storage_scope = storage_info.value()[0];
      }
      tokens.push_back(token);
    }
    token_map_[op] = tokens;
  }

  void VisitExpr_(const CallNode* op) final {
    // create token for the call node.
    CreateToken(op, true);
    // for each input, visit argument token.
    for (Expr arg : op->args) {
      for (StorageToken* tok : GetToken(arg)) {
        tok->ref_counter += 1;
      }
    }
  }

 private:
  // allocator
  support::Arena* arena_;
  Map<Expr, Integer> node_device_map_;
  Map<Expr, Array<String>> node_storage_map_;
};

class StorageAllocator : public StorageAllocaBaseVisitor {
 public:
  // Run storage allocation for a function.
  Map<Expr, runtime::ADT> Plan(const Function& func, const TargetsMap& targets) {
    prototype_ = StorageAllocaInit(&arena_).GetInitTokenMap(func, targets);
    this->Run(func);

    // The value of smap contains two integer arrays where the first array
    // contains the planned storage ids and the second holds the device types.
    Map<Expr, runtime::ADT> smap;
    int num_annotated_nodes = 0;
    int num_nodes = 0;

    for (const auto& kv : token_map_) {
      std::vector<Integer> storage_ids;
      std::vector<Integer> device_types;
      std::vector<String> storage_scopes;
      for (StorageToken* tok : kv.second) {
        if (tok->device_type) {
          num_annotated_nodes++;
        }
        num_nodes++;
        storage_ids.push_back(tok->storage_id);
        device_types.push_back(tok->device_type);
        storage_scopes.push_back(tok->storage_scope);
      }
      std::vector<ObjectRef> fields{
        Array<Integer>{storage_ids}, Array<Integer>{device_types}, Array<String>{storage_scopes}};
      smap.Set(GetRef<Expr>(kv.first), runtime::ADT::Tuple(fields));
    }
    // Either all or none of the nodes should be annotated.
    if (num_annotated_nodes != 0 && num_annotated_nodes != num_nodes) {
      LOG(FATAL) << num_annotated_nodes << " out of " << num_nodes
                 << "expressions are assigned with virtual device types. Either all "
                    "or none of the expressions are expected to be annotated.";
    }
    return smap;
  }

 protected:
  using StorageAllocaBaseVisitor::VisitExpr_;
  // override create token by getting token as prototype requirements.
  void CreateToken(const ExprNode* op, bool can_realloc) final {
    ICHECK(!token_map_.count(op));
    auto it = prototype_.find(op);
    ICHECK(it != prototype_.end());
    std::vector<StorageToken*> tokens;
    for (StorageToken* tok : it->second) {
      // TODO(csullivan): Remove the check on global which
      // prevents memory reuse for texture memory
      if (can_realloc && tok->storage_scope == "global") {
        tokens.push_back(allocator_.Request(tok));
      } else {
        // Allocate a new token,
        StorageToken* allocated_tok = allocator_.Alloc(tok);
        allocated_tok->device_type = tok->device_type;
        // ensure it never get de-allocated.
        allocated_tok->ref_counter += 1;
        tokens.push_back(allocated_tok);
      }
    }
    token_map_[op] = tokens;
  }
  // The call map
  void VisitExpr_(const CallNode* op) final {
    std::vector<StorageToken*> args;
    // for each input, visit argument token.
    for (Expr arg : op->args) {
      for (StorageToken* tok : GetToken(arg)) {
        args.push_back(tok);
      }
    }
    // create token for the call node.
    CreateToken(op, true);
    // check if there is orphaned output that can be released immediately.
    for (StorageToken* tok : token_map_.at(op)) {
      allocator_.CheckForRelease(tok);
    }
    for (StorageToken* tok : args) {
      tok->ref_counter -= 1;
      allocator_.CheckForRelease(tok);
    }
  }

 private:
  class TokenAllocator1D {
  public:
    /*!
     * \brief Request a storage token for a given prototype.
     * \param prototype. The prototype storage token.
     * \return The result token.
     */
    StorageToken* Request(StorageToken* prototype) {
      // calculate the size;
      size_t size = GetMemorySize(prototype);
      // search memory block in [size / match_range_, size * match_range_)
      if (match_range_ == 0) {
        return nullptr;
      }
      auto begin = free_.lower_bound(size / match_range_);
      auto mid = free_.lower_bound(size);
      auto end = free_.upper_bound(size * match_range_);
      // search for memory blocks larger than requested
      for (auto it = mid; it != end; ++it) {
        StorageToken* tok = it->second;
        if (tok->device_type != prototype->device_type) continue;
        ICHECK_EQ(tok->ref_counter, 0);
        // Use exect matching strategy
        tok->max_bytes = std::max(size, tok->max_bytes);
        tok->ref_counter = prototype->ref_counter;
        // find a exact match, erase from map and return
        free_.erase(it);
        return tok;
      }
      // then search for memory blocks smaller than requested space
      for (auto it = mid; it != begin;) {
        --it;
        StorageToken* tok = it->second;
        if (tok->device_type != prototype->device_type) continue;
        ICHECK_EQ(tok->ref_counter, 0);
        // Use exect matching strategy
        tok->max_bytes = std::max(size, tok->max_bytes);
        tok->ref_counter = prototype->ref_counter;
        // erase from map and return
        free_.erase(it);
        return tok;
      }
      return nullptr;
    }
    /*!
     * \brief Alloacte a storage token by consuming prototype
     * \param prototype The prototype token.
     * \param size The size of memory being requested.
     */
    StorageToken* Alloc(StorageToken* prototype, int64_t storage_id) {
      size_t size = GetMemorySize(prototype);
      prototype->max_bytes = size;
      prototype->storage_id = storage_id;
      data_.push_back(prototype);
      return prototype;
    }
    /*!
     * \brief Check if we can release token.
     * \param tok The token to be released.
     */
    void CheckForRelease(StorageToken* tok) {
      ICHECK_GE(tok->storage_id, 0);
      ICHECK_GE(tok->ref_counter, 0);
      if (tok->ref_counter == 0) {
        free_.insert({tok->max_bytes, tok});
      }
    }
    /*!
     * \return totoal number of bytes allocated
     */
    size_t TotalAllocBytes() const {
      size_t total = 0;
      for (const auto* p : data_) {
        total += p->max_bytes;
      }
      return total;
    }
    /*!
     * \brief ceil(size/word_size) to get number of words.
     * \param size The original size.
     * \param word_size The element size.
     */
    static size_t DivRoundUp(size_t size, size_t word_size) {
      return (size + word_size - 1) / word_size;
    }
    /*!
     * \brief Get the memory requirement.
     * \param prototype The prototype token.
     * \return The required memory size.
     */
    size_t GetMemorySize(StorageToken* prototype) {
      const TensorTypeNode* ttype = prototype->ttype;
      ICHECK(ttype != nullptr);
      size_t size = 1;
      for (IndexExpr dim : ttype->shape) {
        const int64_t* pval = tir::as_const_int(dim);
        ICHECK(pval != nullptr) << "Cannot allocate memory symbolic tensor shape " << ttype->shape;
        ICHECK_GE(*pval, 0) << "Cannot allocate memory for tensor with negative shape" << *pval;
        size *= static_cast<size_t>(pval[0]);
      }
      size *= DivRoundUp(ttype->dtype.bits() * ttype->dtype.lanes(), 8);
      return size;
    }
  private:
    // scale used for rough match
    const size_t match_range_{16};
    // free list of storage entry
    std::multimap<size_t, StorageToken*> free_;
    // all the storage resources available
    std::vector<StorageToken*> data_;
  };

  class TokenAllocator2D {
  public:
    /*!
     * \brief Request a storage token for a given prototype.
     * \param prototype. The prototype storage token.
     * \return The result token.
     */
    StorageToken* Request(StorageToken* prototype) {
      auto shape = GetSize2D(prototype);
      int64_t requested_size = shape.height * shape.width;
      int64_t min_added_size = std::numeric_limits<int64_t>::max();
      int64_t min_wasted_size = std::numeric_limits<int64_t>::max();
      int64_t best_storage_id = -1;
      MemBlock best_mem, new_mem;
      for (int64_t free_id : free_list_) {
        MemBlock& cached = blocks_[free_id];
        // Can only reuse texture 2d blocks of the same type
        if (cached.token_->ttype->dtype != prototype->ttype->dtype) {
          continue;
        }
        int64_t cached_size = cached.x_ * cached.y_;
        new_mem.x_ = std::max(cached.x_, shape.width);
        new_mem.y_ = std::max(cached.y_, shape.height);
        int64_t expanded_size = new_mem.x_ * new_mem.y_;
        int64_t added_size = expanded_size - cached_size;
        int64_t wasted_size = expanded_size - requested_size;
        // Prioritize minimization of added size first, then minimize
        // wasted size among blocks which would not require expansion
        if ((min_added_size > 0 && added_size < min_added_size) ||
            (min_added_size == 0 && wasted_size < min_wasted_size)) {
          min_added_size = added_size;
          min_wasted_size = wasted_size;
          best_storage_id = free_id;
          best_mem = new_mem;
        }
      }

      if (min_added_size <= requested_size) {
        best_mem.token_ = blocks_[best_storage_id].token_;
        // Reset the reference counter of the now live token
        best_mem.token_->ref_counter = prototype->ref_counter;
        blocks_[best_storage_id] = best_mem;
        free_list_.erase(best_storage_id);
        return best_mem.token_;
      }
      return nullptr;
    }
    /*!
     * \brief Alloacte a storage token by consuming prototype
     * \param prototype The prototype token.
     * \param size The size of memory being requested.
     */
    StorageToken* Alloc(StorageToken* prototype, int64_t storage_id) {
      auto shape = GetSize2D(prototype);
      MemBlock block;
      block.x_ = shape.width;
      block.y_ = shape.height;
      prototype->storage_id = storage_id;
      block.token_ = prototype;
      blocks_[prototype->storage_id] = block;
      return prototype;
    }
    /*!
     * \brief Check if we can release token.
     * \param tok The token to be released.
     */
    void CheckForRelease(StorageToken* tok) {
      ICHECK_GE(tok->storage_id, 0);
      ICHECK_GE(tok->ref_counter, 0);
      if (tok->ref_counter == 0) {
        free_list_.insert(tok->storage_id);
      }
    }
    /*!
     * \brief Get the texture 2d size requirement
     * \param prototype The prototype token.
     * \return The required texture 2d memory size in (width, height, channel).
     */
    Texture2DShape GetSize2D(StorageToken* prototype) {
      const TensorTypeNode* ttype = prototype->ttype;
      ICHECK(ttype != nullptr);
      size_t axis = runtime::DefaultTextureLayoutSeparator(ttype->shape.size(), prototype->storage_scope);
      struct Shape {
        const Array<PrimExpr>& shape;
        int64_t operator[](size_t i) const { return *tir::as_const_int(shape[i]); }
      };
      return runtime::ApplyTexture2DFlattening<int64_t>(Shape{ttype->shape}, ttype->shape.size(), axis);
    }
  private:
    struct MemBlock {
      StorageToken* token_;
      int64_t x_;
      int64_t y_;
    };

    std::unordered_map<int64_t, MemBlock> blocks_;
    std::unordered_set<int64_t> free_list_;
  };

  class TokenAllocator {
  public:
    StorageToken* Alloc(StorageToken* proto) {
      return Is2DStorage(proto) ? token_2d_.Alloc(proto, storage_ids_++) : token_1d_.Alloc(proto, storage_ids_++);
    }
    StorageToken* Request(StorageToken* proto) {
      StorageToken* token = Is2DStorage(proto) ? token_2d_.Request(proto) : token_1d_.Request(proto);
      return token ? token : this->Alloc(proto);
    }
    void CheckForRelease(StorageToken* tok) {
      return Is2DStorage(tok) ? token_2d_.CheckForRelease(tok) : token_1d_.CheckForRelease(tok);
    }
    static bool Is2DStorage(StorageToken* tok) { return relay::Is2DStorage(tok->storage_scope); }

  private:
    int64_t storage_ids_{0};
    TokenAllocator1D token_1d_;
    TokenAllocator2D token_2d_;
  };

  // allocator
  support::Arena arena_;
  /*! \brief internal prototype token map */
  std::unordered_map<const ExprNode*, std::vector<StorageToken*> > prototype_;
  /*! \brief token allocator for optimizing 1d and 2d token alloc requests */
  TokenAllocator allocator_;
};

Map<Expr, runtime::ADT> GraphPlanMemory(const Function& func, const TargetsMap& targets) {
  return StorageAllocator().Plan(func, targets);
}

TVM_REGISTER_GLOBAL("relay.backend.GraphPlanMemory").set_body_typed(GraphPlanMemory);

}  // namespace relay
}  // namespace tvm
