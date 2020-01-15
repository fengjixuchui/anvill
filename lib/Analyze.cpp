/*
 * Copyright (c) 2019 Trail of Bits, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "anvill/Analyze.h"

#include <glog/logging.h>

#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/Utils/Local.h>

#include <remill/BC/Optimizer.h>
#include <remill/BC/Util.h>

#include "anvill/Decl.h"
#include "anvill/Program.h"

namespace anvill {
namespace {

struct Cell {
  llvm::Function *containing_func{nullptr};
  llvm::Type *type{nullptr};
  llvm::User *user{nullptr};
  llvm::ConstantInt *address_val{nullptr};
  uint64_t address_const{0};
  unsigned size{0};
  bool is_load{false};
  bool is_store{false};
  bool is_volatile{false};
  bool is_atomic{false};
};

static void UnfoldConstantExpressions(llvm::Instruction *inst);

// Unfold constant expressions by expanding them into their relevant
// instructions inline in the original module. This lets us deal uniformly
// in terms of instructions.
static void UnfoldConstantExpressions(llvm::Instruction *inst,
                                      llvm::Use &use) {
  const auto val = use.get();
  if (auto ce = llvm::dyn_cast<llvm::ConstantExpr>(val)) {
    const auto ce_inst = ce->getAsInstruction();
    ce_inst->insertBefore(inst);
    UnfoldConstantExpressions(ce_inst);
    use.set(ce_inst);
  }
}

// Looks for any constant expressions in the operands of `inst` and unfolds
// them into other instructions in the same block.
void UnfoldConstantExpressions(llvm::Instruction *inst) {
  for (auto &use : inst->operands()) {
    UnfoldConstantExpressions(inst, use);
  }
  if (llvm::CallInst *call = llvm::dyn_cast<llvm::CallInst>(inst)) {
    for (llvm::Use &use : call->arg_operands()) {
      UnfoldConstantExpressions(inst, use);
    }
  }
}

// Expand a type into `out_types`, so that we can iterate over the elements
// more easily.
static void FlattenTypeInto(llvm::Type *type,
                            std::vector<llvm::Type *> &out_types) {

  if (auto arr_type = llvm::dyn_cast<llvm::ArrayType>(type)) {
    const auto elem_type = arr_type->getArrayElementType();
    const auto num_elems = arr_type->getArrayNumElements();
    for (auto i = 0U; i < num_elems; ++i) {
      FlattenTypeInto(elem_type, out_types);
    }

  } else if (auto rec_type = llvm::dyn_cast<llvm::StructType>(type)) {
    for (auto elem_type : rec_type->elements()) {
      FlattenTypeInto(elem_type, out_types);
    }

  } else if (auto vec_type = llvm::dyn_cast<llvm::VectorType>(type)) {
    const auto elem_type = vec_type->getVectorElementType();
    const auto num_elems = vec_type->getVectorNumElements();
    for (auto i = 0U; i < num_elems; ++i) {
      FlattenTypeInto(elem_type, out_types);
    }

  } else {
    out_types.push_back(type);
  }
}

// Recursively scans through llvm Values and tries to find uses of
// constant integers. The use case of this is to find uses of
// stack variables and global variables.
static void FindConstantBases(
    llvm::User *user,
    llvm::Value *val,
    std::unordered_map<llvm::User *, std::vector<llvm::ConstantInt *>> &bases,
    std::set<std::pair<llvm::User *, llvm::Value *>> &seen) {

  auto seen_size = seen.size();
  seen.emplace(user, val);
  if (seen_size == seen.size()) {
    return;
  }

  if (auto const_val = llvm::dyn_cast<llvm::ConstantInt>(val)) {
    bases[user].push_back(const_val);

  } else if (auto inst = llvm::dyn_cast<llvm::Instruction>(val)) {
    switch (inst->getOpcode()) {
      case llvm::Instruction::PHI: {
        auto node = llvm::dyn_cast<llvm::PHINode>(inst);
        for (const auto &operand : node->incoming_values()) {
          FindConstantBases(node, operand.get(), bases, seen);
        }
        break;
      }
      case llvm::Instruction::GetElementPtr:
      case llvm::Instruction::BitCast:
      case llvm::Instruction::IntToPtr:
      case llvm::Instruction::PtrToInt:
      case llvm::Instruction::ZExt:
      case llvm::Instruction::SExt:
        FindConstantBases(inst, inst->getOperand(0), bases, seen);
        break;
      case llvm::Instruction::Sub:
      case llvm::Instruction::Add:
        FindConstantBases(inst, inst->getOperand(0), bases, seen);
        break;

      // TODO(pag): Think through `Add`, `And`, and `Or` more.

//      case llvm::Instruction::Add:
//      case llvm::Instruction::And:
//      case llvm::Instruction::Or: {
//        auto lhs = inst->getOperand(0);
//        auto rhs = inst->getOperand(1);
//      }
//
//        FindConstantBases(inst, inst->getOperand(0), bases, seen);
//        FindConstantBases(inst, , bases, seen);
//        break;
      default:
        break;
    }
  }
}

// Get the type that is the source of `val`.
//
// NOTE(pag): `val` is an integer or float type.
static llvm::Type *GetUpstreamType(llvm::Value *val) {
  if (auto bc_inst = llvm::dyn_cast<llvm::BitCastInst>(val)) {
    return GetUpstreamType(bc_inst->getOperand(0));

  } else if (auto ptr_inst = llvm::dyn_cast<llvm::PtrToIntInst>(val)) {
    return ptr_inst->getOperand(0)->getType();

  } else {
    return val->getType();
  }
}

// Get the type that `val` ends up being converted to.
//
// NOTE(pag): `val` in an integer or floating point type.
static llvm::Type *GetDownstreamType(llvm::Value *val) {
  for (auto &use : val->uses()) {
    auto down_val = use.get();
    if (llvm::isa<llvm::BitCastInst>(down_val) ||
        llvm::isa<llvm::IntToPtrInst>(down_val)) {
      return GetDownstreamType(down_val);
    }
  }
  return val->getType();
}

// Get the type that is the source of `val`.
//
// NOTE(pag): `val` is a pointer type.
static llvm::Type *GetUpstreamTypeFromPointer(llvm::Value *val) {
  auto stripped = val->stripPointerCasts();
  return GetDownstreamType(stripped);
}

// Locate references to memory locations.
static void FindMemoryReferences(
    const Program &program, llvm::Function &func,
    llvm::SmallPtrSetImpl<llvm::Type* > &size_checked,
    std::vector<Cell> &stack_cells, std::vector<Cell> &global_cells) {

  llvm::DataLayout dl(func.getParent());

  std::vector<llvm::CallInst *> calls;
  std::unordered_map<llvm::User *, std::vector<llvm::ConstantInt *>> user_bases;
  std::set<std::pair<llvm::User *, llvm::Value *>> seen;

  for (auto &block : func) {
    for (auto &inst : block) {

      Cell cell;
      cell.containing_func = &func;

      llvm::Value *address_val = nullptr;

      // We need to make sure that we can replace constants with
      // allocas (or GEPs into them) casted to integers. This won't
      // be possibly if the constants we find are inside of constant
      // expressions.
      UnfoldConstantExpressions(&inst);

      if (auto call_inst = llvm::dyn_cast<llvm::CallInst>(&inst)) {

        const auto intrinsic = call_inst->getCalledFunction();
        if (!intrinsic) {
          continue;
        }

        auto name = intrinsic->getName();
        if (!name.startswith("__remill")) {
          calls.push_back(call_inst);
          continue;
        }

        // Loads.
        if (name.startswith("__remill_read_memory_")) {
          cell.is_load = true;
          cell.type = GetDownstreamType(call_inst);

        // Stores.
        } else if (name.startswith("__remill_write_memory_")) {
          cell.is_store = true;
          cell.type = GetUpstreamType(call_inst->getArgOperand(2));

        // Compare-and-swap.
        } else if (name.startswith("__remill_compare_exchange_")) {
          cell.is_load = true;
          cell.is_store = true;
          cell.is_atomic = true;
          cell.type = GetUpstreamType(call_inst->getArgOperand(3));

        // Fetch-and-update.
        } else if (name.startswith("__remill_fetch_and_")) {
          cell.is_load = true;
          cell.is_store = true;
          cell.is_atomic = true;
          cell.type = GetUpstreamTypeFromPointer(
              call_inst->getArgOperand(2));

        // Loads from memory mapped I/O.
        } else if (name.startswith("__remill_read_io_port_")) {
          cell.is_load = true;
          cell.is_volatile = true;
          cell.type = GetDownstreamType(call_inst);

        // Stores to memory mapped I/O.
        } else if (name.startswith("__remill_write_io_port_")) {
          cell.is_store = true;
          cell.is_volatile = true;
          cell.type = GetUpstreamType(call_inst->getArgOperand(2));

        } else {
          continue;
        }

        address_val = call_inst->getArgOperand(1);

      // Integer to pointer cast.
      } else if (auto ptr_inst = llvm::dyn_cast<llvm::IntToPtrInst>(&inst)) {
        address_val = ptr_inst->getOperand(0);
        cell.type = ptr_inst->getType()->getPointerElementType();

      // Bitcast.
      } else if (auto bitcast_inst = llvm::dyn_cast<llvm::BitCastInst>(&inst)) {
        if (bitcast_inst->getType()->isPointerTy()) {
          address_val = bitcast_inst->stripPointerCasts();
          cell.type = GetUpstreamType(bitcast_inst);

        } else {
          continue;
        }

      // TODO(pag): GEPs, others?
      } else {
        continue;
      }
      if (cell.type->isFunctionTy()) {
        cell.size = dl.getPointerSize(0);

      } else {
        CHECK(cell.type->isSized(&size_checked))
                << "Unable to determine size of type: "
                << remill::LLVMThingToString(cell.type);

        cell.size = static_cast<unsigned>(dl.getTypeAllocSize(cell.type));
      }

      user_bases.clear();
      seen.clear();
      FindConstantBases(&inst, address_val, user_bases, seen);

      for (auto &user_base_list : user_bases) {
        cell.user = user_base_list.first;
        for (auto base : user_base_list.second) {
          cell.address_val = base;
          cell.address_const = base->getZExtValue();
          auto byte = program.FindByte(cell.address_const);
          if (byte.IsStack()) {
            stack_cells.push_back(cell);
          } else {
            global_cells.push_back(cell);
          }
        }
      }
    }
  }

  // There may be calls to other functions, and the arguments might have
  // constant expression pointer casts or other stuff, so lets go inspect
  // those.
  for (auto call_inst : calls) {
    for (auto &arg : call_inst->arg_operands()) {
      const auto val = arg.get();
      if (!val->getType()->isPointerTy()) {
        continue;
      }

      Cell cell;
      cell.containing_func = &func;
      cell.type = val->getType()->getPointerElementType();
      if (cell.type->isFunctionTy()) {
        cell.size = dl.getPointerSize(0);
      } else {
        CHECK(cell.type->isSized(&size_checked))
                << "Unable to determine size of type: "
                << remill::LLVMThingToString(cell.type);

        cell.size = static_cast<unsigned>(dl.getTypeAllocSize(cell.type));
      }

      user_bases.clear();
      seen.clear();

      FindConstantBases(call_inst, val, user_bases, seen);
      for (auto &user_base_list : user_bases) {
        cell.user = user_base_list.first;
        for (auto base : user_base_list.second) {
          cell.address_val = base;
          cell.address_const = base->getZExtValue();

          auto byte = program.FindByte(cell.address_const);
          if (byte.IsStack()) {
            stack_cells.push_back(cell);
          } else {
            global_cells.push_back(cell);
          }
        }
      }
    }
  }
}

// Replace a memory barrier intrinsic.
//
// TODO(pag): Consider calling something real.
static void ReplaceBarrier(
    llvm::Module &module, const char *name) {
  auto func = module.getFunction(name);
  if (!func) {
    return;
  }

  CHECK(func->isDeclaration())
      << "Cannot lower already implemented memory intrinsic " << name;

  auto callers = remill::CallersOf(func);
  for (auto call_inst : callers) {
    auto mem_ptr = call_inst->getArgOperand(0);
    call_inst->replaceAllUsesWith(mem_ptr);
    call_inst->eraseFromParent();
  }
}

// Lower a memory read intrinsic into a `load` instruction.
static void ReplaceMemReadOp(
    llvm::Module &module, const char *name, llvm::Type *val_type,
    std::unordered_set<llvm::Value *> &pointers) {
  auto func = module.getFunction(name);
  if (!func) {
    return;
  }

  CHECK(func->isDeclaration())
      << "Cannot lower already implemented memory intrinsic " << name;

  auto callers = remill::CallersOf(func);
  for (auto call_inst : callers) {
    auto addr = call_inst->getArgOperand(1);

    llvm::IRBuilder<> ir(call_inst);
    llvm::Value *ptr = nullptr;
    if (auto as_int = llvm::dyn_cast<llvm::PtrToIntInst>(addr)) {
      pointers.insert(as_int->getPointerOperand());
      ptr = ir.CreateBitCast(
          as_int->getPointerOperand(),
          llvm::PointerType::get(val_type, as_int->getPointerAddressSpace()));

    } else {
      ptr = ir.CreateIntToPtr(addr, llvm::PointerType::get(val_type, 0));
    }

    pointers.insert(ptr);

    llvm::Value *val = ir.CreateLoad(ptr);
    if (val_type->isX86_FP80Ty()) {
      val = ir.CreateFPTrunc(val, func->getReturnType());
    }
    call_inst->replaceAllUsesWith(val);
  }
  for (auto call_inst : callers) {
    call_inst->eraseFromParent();
  }
}

// Lower a memory write intrinsic into a `store` instruction.
static void ReplaceMemWriteOp(
    llvm::Module &module, const char *name, llvm::Type *val_type,
    std::unordered_set<llvm::Value *> &pointers) {
  auto func = module.getFunction(name);
  if (!func) {
    return;
  }

  CHECK(func->isDeclaration())
          << "Cannot lower already implemented memory intrinsic " << name;

  auto callers = remill::CallersOf(func);

  for (auto call_inst : callers) {
    auto mem_ptr = call_inst->getArgOperand(0);
    auto addr = call_inst->getArgOperand(1);
    auto val = call_inst->getArgOperand(2);

    llvm::IRBuilder<> ir(call_inst);

    llvm::Value *ptr = nullptr;
    if (auto as_int = llvm::dyn_cast<llvm::PtrToIntInst>(addr)) {
      pointers.insert(as_int->getPointerOperand());
      ptr = ir.CreateBitCast(
          as_int->getPointerOperand(),
          llvm::PointerType::get(val_type, as_int->getPointerAddressSpace()));
    } else {
      ptr = ir.CreateIntToPtr(addr, llvm::PointerType::get(val_type, 0));
    }

    pointers.insert(ptr);

    if (val_type->isX86_FP80Ty()) {
      val = ir.CreateFPExt(val, val_type);
    }

    ir.CreateStore(val, ptr);
    call_inst->replaceAllUsesWith(mem_ptr);
  }
  for (auto call_inst : callers) {
    call_inst->eraseFromParent();
  }
}

static void LowerMemOps(llvm::Module &module,
                        std::unordered_set<llvm::Value *> &pointers) {
  auto &context = module.getContext();

  ReplaceMemReadOp(module, "__remill_read_memory_8",
                   llvm::Type::getInt8Ty(context), pointers);
  ReplaceMemReadOp(module, "__remill_read_memory_16",
                   llvm::Type::getInt16Ty(context), pointers);
  ReplaceMemReadOp(module, "__remill_read_memory_32",
                   llvm::Type::getInt32Ty(context), pointers);
  ReplaceMemReadOp(module, "__remill_read_memory_64",
                   llvm::Type::getInt64Ty(context), pointers);
  ReplaceMemReadOp(module, "__remill_read_memory_f32",
                   llvm::Type::getFloatTy(context), pointers);
  ReplaceMemReadOp(module, "__remill_read_memory_f64",
                   llvm::Type::getDoubleTy(context), pointers);

  ReplaceMemWriteOp(module, "__remill_write_memory_8",
                    llvm::Type::getInt8Ty(context), pointers);
  ReplaceMemWriteOp(module, "__remill_write_memory_16",
                    llvm::Type::getInt16Ty(context), pointers);
  ReplaceMemWriteOp(module, "__remill_write_memory_32",
                    llvm::Type::getInt32Ty(context), pointers);
  ReplaceMemWriteOp(module, "__remill_write_memory_64",
                    llvm::Type::getInt64Ty(context), pointers);
  ReplaceMemWriteOp(module, "__remill_write_memory_f32",
                    llvm::Type::getFloatTy(context), pointers);
  ReplaceMemWriteOp(module, "__remill_write_memory_f64",
                    llvm::Type::getDoubleTy(context), pointers);

  ReplaceMemReadOp(module, "__remill_read_memory_f80",
                   llvm::Type::getX86_FP80Ty(context), pointers);
  ReplaceMemWriteOp(module, "__remill_write_memory_f80",
                    llvm::Type::getX86_FP80Ty(context), pointers);

  ReplaceBarrier(module, "__remill_barrier_load_load");
  ReplaceBarrier(module, "__remill_barrier_load_store");
  ReplaceBarrier(module, "__remill_barrier_store_load");
  ReplaceBarrier(module, "__remill_barrier_store_store");
  ReplaceBarrier(module, "__remill_barrier_atomic_begin");
  ReplaceBarrier(module, "__remill_barrier_atomic_end");
}

// Build up a list of indexes into `type` to get as near as possible to
// `remainder`, which should always be less than the size of `type`. Returns
// the difference between what we indexed to and `remainder`.
static uint64_t GetIndexesInto(
    const llvm::DataLayout &dl, llvm::Type *type,
    std::vector<llvm::Value *> &indexes, uint64_t remainder) {
  uint64_t offset = 0;
  auto index_type = indexes[0]->getType();

  if (auto arr_type = llvm::dyn_cast<llvm::ArrayType>(type)) {
    const auto elem_type = arr_type->getArrayElementType();
    const auto num_elems = arr_type->getArrayNumElements();
    const auto elem_size = dl.getTypeAllocSize(elem_type);
    auto i = 0U;
    for (; i < num_elems && (offset + elem_size) <= remainder; ++i) {
      offset += elem_size;
    }

    indexes.push_back(llvm::ConstantInt::get(index_type, i));
    return GetIndexesInto(dl, elem_type, indexes, remainder - offset);

  } else if (auto vec_type = llvm::dyn_cast<llvm::VectorType>(type)) {
    const auto elem_type = vec_type->getVectorElementType();
    const auto num_elems = vec_type->getVectorNumElements();
    const auto elem_size = dl.getTypeAllocSize(elem_type);
    auto i = 0U;
    for (; i < num_elems && (offset + elem_size) <= remainder; ++i) {
      offset += elem_size;
    }

    indexes.push_back(llvm::ConstantInt::get(index_type, i));
    return GetIndexesInto(dl, elem_type, indexes, remainder - offset);

  } else if (auto rec_type = llvm::dyn_cast<llvm::StructType>(type)) {
    auto i = 0U;
    for (auto elem_type : rec_type->elements()) {
      auto elem_size = dl.getTypeAllocSize(elem_type);
      if ((offset + elem_size) <= remainder) {
        ++i;
        offset += elem_size;
      } else {
        indexes.push_back(llvm::ConstantInt::get(index_type, i));
        return GetIndexesInto(dl, elem_type, indexes,
                              remainder - offset);
      }
    }

    LOG(FATAL)
        << "Fell off the end of " << remill::LLVMThingToString(rec_type);

  } else {
    return remainder;
  }
}

// Recover stack memory accesses. Each of the `Cell`s in `cells` is some use
// of an integral memory address that falls in the range of our fake stack, and
// we want replace them with a pointer into an `alloca`d stack. We need to look
// at the accesses to the stack and do our best to create a structure type that
// represents the stack frame itself.
static void RecoverStackMemoryAccesses(
    const Program &program, llvm::Function *func,
    const std::vector<Cell> &cells,
    std::unordered_set<llvm::Value *> &pointers) {

  auto sp = program.InitialStackPointer();
  if (!sp) {
    LOG(FATAL) << "Found invalid initial stack pointer";
  }

  auto &context = func->getContext();
  auto i8_type = llvm::Type::getInt8Ty(context);
  auto i8_ptr_type = llvm::PointerType::get(i8_type, 0);
  auto i32_type = llvm::Type::getInt32Ty(context);

  uint64_t min_stack_address = *sp;
  uint64_t max_stack_address = min_stack_address;

  for (const auto &cell : cells) {
    min_stack_address = std::min(min_stack_address, cell.address_const);
    max_stack_address = std::max(max_stack_address,
                                 cell.address_const + cell.size);
  }

  // NOTE(pag): This is based off of the amd64 ABI redzone, and
  //            hopefully represents an appropriate redzone size.
  //
  //            Consider having additional info in the `FunctionDecl`
  //            for either frame size or redzone size.
  uint64_t running_addr = min_stack_address - 128;
  std::vector<llvm::Type *> types;

  for (const auto &cell : cells) {
    if (running_addr < cell.address_const) {
      auto padding_bytes = cell.address_const - running_addr;
      types.push_back(llvm::ArrayType::get(i8_type, padding_bytes));
      running_addr = cell.address_const;

      // NOTE(pag): This assumes downward stack growth.
    } else if (running_addr > cell.address_const) {
      LOG(ERROR)
          << "SKIPPING " << std::hex << " " << cell.address_const << std::dec
          << " " << remill::LLVMThingToString(cell.type) << " size "
          << cell.size;
      continue;
    }

    types.push_back(cell.type);
    running_addr -= cell.size;
  }

  llvm::IRBuilder<> ir(&(func->getEntryBlock().front()));
  std::stringstream frame_ss;
  const auto frame_type = llvm::StructType::create(
      context, types, func->getName().str() + ".frame_type", false);
  const auto frame = ir.CreateAlloca(frame_type);
  llvm::Value *i8_frame = nullptr;

  pointers.insert(frame);
  std::unordered_map<uint64_t, llvm::Value *> offset_cache;

  for (const auto &cell : cells) {
    for (auto &use : cell.address_val->uses()) {
      if (use.getUser() != cell.user) {
        continue;
      }

      auto &gep = offset_cache[cell.address_const];
      if (!gep) {
        llvm::Value *indexes[] = {
            llvm::ConstantInt::get(
                i32_type, cell.address_const - min_stack_address)
        };

        if (!i8_frame) {
          i8_frame = ir.CreateBitCast(frame, i8_ptr_type);
          pointers.insert(i8_frame);
        }

        gep = ir.CreateInBoundsGEP(i8_type, i8_frame, indexes);
        pointers.insert(gep);
      }

      use.set(ir.CreatePtrToInt(gep, cell.address_val->getType()));
    }
  }
}

// Try to partition what we know about memory into global variables, and then
// add them to `nearby` as new globals.
//
// Returns a new value for `max_var_size`, which represents the size of the
// largest declared/defined global variable.
size_t DeclareMissingGlobals(
    llvm::Module &module, std::vector<Cell> &global_cells,
    std::map<uint64_t, llvm::GlobalValue *> &nearby, size_t max_var_size) {

  llvm::DataLayout dl(&module);
  auto &context = module.getContext();

  auto cell_it = global_cells.begin();
  while (cell_it != global_cells.end()) {

    std::vector<llvm::Type *> types;
    types.push_back(cell_it->type);

    const auto addr = cell_it->address_const;
    auto next_addr = addr + cell_it->size;
    auto is_packed = false;
    ++cell_it;

    while (cell_it != global_cells.end()) {

      // Next cell is covered in this cell.
      if (cell_it->address_const < next_addr) {
        auto maybe_next_addr = cell_it->address_const + cell_it->size;

        // Ugh, it actually straddles this cell, add in some padding.
        for (; next_addr < maybe_next_addr; ++next_addr) {
          types.push_back(llvm::Type::getInt8Ty(context));
          is_packed = true;
        }

        ++cell_it;
        continue;

        // Next cell is adjacent to this cell, extend us.
      } else if (cell_it->address_const == next_addr) {
        next_addr += next_addr + cell_it->size;
        types.push_back(cell_it->type);
        ++cell_it;
        continue;

        // There is a gap, we have the end of a global variable.
      } else {
        break;
      }
    }

    std::stringstream ss;
    ss << "data_" << std::hex << addr;

    auto global_type = llvm::StructType::create(
        context, types, ss.str() + ".type", is_packed);
    auto global_size = dl.getTypeAllocSize(global_type);
    auto &global = nearby[addr];

    if (global_size > max_var_size) {
      max_var_size = global_size;
    }

    if (auto existing_global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(global)) {
      if (dl.getTypeAllocSize(existing_global->getValueType()) < global_size) {
        auto name = existing_global->getName();
        LOG(WARNING)
            << "Found overlapping global variables '" << ss.str()
            << "' and '" << name.str() << "'";

        // TODO(pag): Make `existing_global` and alias of `global`.
        existing_global->eraseFromParent();
        global->setName(name);
      }

    } else {
      global = new llvm::GlobalVariable(
          module, global_type, false, llvm::GlobalValue::ExternalLinkage,
          nullptr, ss.str());
    }
  }

  return max_var_size;
}

// Recover uses of global variables.
static void RecoverGlobalVariableAccesses(
    llvm::DataLayout &dl, std::vector<Cell> &global_cells,
    std::map<uint64_t, llvm::GlobalValue *> &nearby,
    size_t max_var_size) {

  for (auto &cell : global_cells) {
    if (!max_var_size) {
      break;  // No global variables.
    }

    llvm::IRBuilder<> ir(&(cell.containing_func->getEntryBlock().front()));

    for (auto &use : cell.address_val->uses()) {
      if (use.getUser() != cell.user) {
        continue;
      }

      // Best case, perfect match against something we have.
      if (nearby.count(cell.address_const)) {
        use.set(ir.CreatePtrToInt(
            nearby[cell.address_const], cell.address_val->getType()));

        // Go try to find the one containing this cell.
      } else {
        uint64_t min_scan = 0;
        if (cell.address_const > max_var_size) {
          min_scan = cell.address_const - max_var_size + 1;
        }

        // Search around for the closest global variable.
        for (auto a = min_scan; a < cell.address_const; ++a) {
          auto near_a_it = nearby.find(a);
          if (near_a_it == nearby.end()) {
            continue;
          }

          // Don't let us find nearby functions, displacing a bitcode function
          // doesn't make sense.
          auto near_a = llvm::dyn_cast<llvm::GlobalVariable>(
              near_a_it->second);
          if (!near_a) {
            continue;
          }

          auto near_a_type = near_a->getValueType();
          auto near_a_size = dl.getTypeAllocSize(near_a_type);

          // The global doesn't include our cell.
          if ((a + near_a_size) < cell.address_const) {
            continue;
          }

          const auto addr_type = cell.address_val->getType();
          use.set(ir.CreateAdd(
              ir.CreatePtrToInt(near_a, addr_type),
              llvm::ConstantInt::get(
                  addr_type, cell.address_const - a, false)));
          break;
        }
      }
    }
  }
}

// Given a pointer `ptr`, look through its uses and see if it is casted to
// and integer and then used in an addition instruction. We then try to replace
// that pattern with a mix of GEPs and bitcasts.
static bool TransformPattern_PTI_Add(
    llvm::DataLayout &dl, llvm::Value *ptr,
    std::vector<llvm::Instruction *> &to_remove) {
  auto changed = false;
  auto type = ptr->getType()->getPointerElementType();
  auto &context = type->getContext();
  auto i32_type = llvm::Type::getInt32Ty(context);
  auto size = dl.getTypeAllocSize(type);
  std::vector<llvm::Value *> indexes;

  for (auto &use : ptr->uses()) {
    const auto ptr_to_int = llvm::dyn_cast<llvm::PtrToIntOperator>(use.getUser());
    if (!ptr_to_int) {
      continue;
    }

    auto addr_type = ptr_to_int->getType();

    for (auto &pti_use : ptr_to_int->uses()) {
      const auto add = llvm::dyn_cast<llvm::AddOperator>(pti_use.getUser());
      if (!add) {
        continue;
      }
      if (!add->hasNUsesOrMore(1)) {
        return false;
      }

      llvm::Value *disp = nullptr;
      if (add->getOperand(0) == ptr_to_int) {
        disp = add->getOperand(1);
      } else {
        disp = add->getOperand(0);
      }

      auto ci = llvm::dyn_cast<llvm::ConstantInt>(disp);
      if (!ci) {
        // TODO(pag): Some kind of pattern matching, e.g. look for things like
        //            a * ci_size for array indexing, perhaps.
        continue;
      }

      uint64_t disp_val = ci->getZExtValue();
      uint64_t base_index = disp_val / size;
      uint64_t remainder = disp_val % size;

      indexes.clear();
      indexes.push_back(llvm::ConstantInt::get(i32_type, base_index));
      remainder = GetIndexesInto(dl, type, indexes, remainder);

      llvm::Type *goal_type = nullptr;
      uint64_t goal_index = 0;

      // If we won't be able to directly index to the thing, then we'll try to
      // bitcast to something else. Lets see if we can find a good bitcast
      // candidate type, otherwise fall back to an `i8*`.
      if (remainder) {
        goal_type = GetDownstreamType(add);
        if (auto goal_ptr_type = llvm::dyn_cast<llvm::PointerType>(goal_type)) {
          auto el_type = goal_ptr_type->getElementType();
          auto el_size = dl.getTypeAllocSize(el_type);
          if (remainder % el_size) {
            goal_type = llvm::Type::getInt8PtrTy(
                context, goal_ptr_type->getAddressSpace());
            goal_index = remainder;
          } else {
            goal_index = remainder / el_size;
          }
        } else {
          goal_type = llvm::Type::getInt8PtrTy(context, 0);
          goal_index = remainder;
        }
      }

      llvm::Value *gep = nullptr;
      if (auto global = llvm::dyn_cast<llvm::GlobalVariable>(ptr)) {
        auto gep1 = llvm::ConstantExpr::getGetElementPtr(
            nullptr, global, indexes);

        if (remainder) {
          indexes.clear();
          indexes.push_back(llvm::ConstantInt::get(i32_type, goal_index));
          gep1 = llvm::ConstantExpr::getBitCast(gep1, goal_type);
          gep1 = llvm::ConstantExpr::getGetElementPtr(nullptr, gep1, indexes);
        }

        gep = llvm::ConstantExpr::getPtrToInt(gep1, addr_type);

      } else if (llvm::isa<llvm::Argument>(ptr) ||
                 llvm::isa<llvm::Instruction>(ptr)) {
        auto insert_loc = llvm::dyn_cast<llvm::Instruction>(add);
        gep = llvm::GetElementPtrInst::Create(
            nullptr, ptr, indexes, "", insert_loc);
        if (remainder) {
          indexes.clear();
          indexes.push_back(llvm::ConstantInt::get(i32_type, goal_index));
          gep = new llvm::BitCastInst(gep, goal_type, "", insert_loc);
          gep = llvm::GetElementPtrInst::Create(
              nullptr, ptr, indexes, "", insert_loc);
        }
        gep = new llvm::PtrToIntInst(gep, addr_type, "", insert_loc);
      }

      if (!gep || gep == add) {
        LOG(ERROR)
            << "Could not GEP into " << remill::LLVMThingToString(ptr);
        continue;
      }

      add->replaceAllUsesWith(gep);
      changed = true;

      if (auto add_inst = llvm::dyn_cast<llvm::BinaryOperator>(add)) {
        to_remove.push_back(add_inst);
      }
    }
  }

  return changed;
}

// Transform a pattern of PHI nodes whose values are all `intoptr` into PHI
// nodes that operate on pointers and then produce an `inttoptr` value. This
// is basically trying to sink `inttoptr`s to occur after PHI nodes, rather
// than before them.
static bool TransformPattern_IntToPtr_PHI(
    llvm::PHINode *phi, std::unordered_set<llvm::Value *> &pointers,
    std::vector<llvm::Instruction *> &to_remove) {

  if (!phi->hasNUsesOrMore(1)) {
    return false;
  }

  llvm::Type *last_pointer_type = nullptr;
  for (auto &use : phi->incoming_values()) {
    if (auto pti = llvm::dyn_cast<llvm::PtrToIntOperator>(use.get())) {
      last_pointer_type = pti->getPointerOperandType();
      CHECK(last_pointer_type->isPointerTy());
    } else {
      return false;
    }
  }

  // Try to find the destination type, otherwise use `last_pointer_type`.
  auto ideal_type = GetDownstreamType(phi);
  if (!ideal_type->isPointerTy()) {
    ideal_type = last_pointer_type;
  }

  llvm::IRBuilder<> ir(phi);
  auto new_phi = ir.CreatePHI(ideal_type, phi->getNumIncomingValues());
  pointers.insert(new_phi);

  for (auto &use : phi->incoming_values()) {
    auto block = phi->getIncomingBlock(use);
    auto pti = llvm::dyn_cast<llvm::PtrToIntOperator>(use.get());
    ir.SetInsertPoint(block->getTerminator());
    auto new_val = ir.CreateBitCast(pti->getPointerOperand(), ideal_type);
    new_phi->addIncoming(new_val, block);
  }

  ir.SetInsertPoint(phi);
  auto new_int_version = ir.CreatePtrToInt(new_phi, phi->getType());
  phi->replaceAllUsesWith(new_int_version);
  to_remove.push_back(phi);

  return true;
}

}  // namespace

// Recover higher-level memory accesses in the lifted functions declared
// in `program` and defined in `module`.
void RecoverMemoryAccesses(const Program &program, llvm::Module &module) {

  llvm::DataLayout dl(&module);
  std::map<uint64_t, llvm::GlobalValue *> nearby;

  // Go collect type information for all memory accesses.
  std::unordered_map<llvm::Function *, std::vector<Cell>> stack_cells;
  std::vector<Cell> global_cells;

  llvm::SmallPtrSet<llvm::Type *, 32> size_checked;

  program.ForEachFunction([&] (const FunctionDecl *decl) -> bool {
    auto &entity = nearby[decl->address];
    if (!entity) {
      if (const auto func = decl->DeclareInModule(module)) {
        entity = func;
        if (!func->isDeclaration()) {
          FindMemoryReferences(
              program, *func, size_checked,
              stack_cells[func], global_cells);
        }
      }
    } else {
      LOG(WARNING)
          << "Multiple entities defined at address "
          << std::hex << decl->address << std::dec;
    }
    return true;
  });

  // Global cells are a bit different. Really, they are about being
  // "close" enough to the real thing
  program.ForEachVariable([&] (const GlobalVarDecl *decl) -> bool {
    auto &var = nearby[decl->address];
    if (!var) {
      var = decl->DeclareInModule(module);
    } else {
      LOG(WARNING)
          << "Multiple entities defined at address "
          << std::hex << decl->address << std::dec;
    }
    return true;
  });

  std::unordered_map<uint64_t, std::unordered_map<llvm::Type *, unsigned>> popularity;

  // Order cells to prefer wider types over smaller types, or wider aligned
  // types over lesser aligned types.
  auto order_cells = [&dl, &popularity] (const Cell &a, const Cell &b) -> bool {
    if (a.address_const < b.address_const) {
      return true;
    } else if (a.address_const > b.address_const) {
      return false;
    } else if (a.size == b.size) {
      if (a.type == b.type) {
        return false;
      } else {

        // If one of the types is a pointer type, then make it ordered first.
        // We give preference to pointer types when possible so that we have
        // fewer integer-to-pointer casts later on.
        const auto a_is_ptr = a.type->isPointerTy();
        const auto b_is_ptr = b.type->isPointerTy();
        if (a_is_ptr != b_is_ptr) {
          return a_is_ptr;
        }

        auto a_align = dl.getABITypeAlignment(a.type);
        auto b_align = dl.getABITypeAlignment(b.type);
        if (a_align == b_align) {
          return popularity[a.address_const][a.type] >
                 popularity[a.address_const][b.type];
        } else {
          return a_align > b_align;
        }
      }
    } else {
      return a.size > b.size;
    }
  };

  std::unordered_set<llvm::Value *> pointers;

  // Scan through the stack cells and try to compute bounds on the stack frame
  // so that we can create a structure representing the stack frame.
  for (auto &func_stack_cells : stack_cells) {
    auto func = func_stack_cells.first;
    auto &cells = func_stack_cells.second;

    popularity.clear();
    for (const auto &cell : cells) {
      popularity[cell.address_const][cell.type] += 1;
    }

    std::sort(cells.begin(), cells.end(), order_cells);
    RecoverStackMemoryAccesses(program, func, cells, pointers);
  }

  popularity.clear();

  // Go collect basic type popularity info across all functions but for
  // things that look like global variables.
  auto max_popularity = 0u;
  for (auto &cell : global_cells) {
    popularity[cell.address_const][cell.type] += 1;
    max_popularity += 1;
  }

  size_t max_var_size = 0;

  // Go through the actual declared types in the global variables and add to
  // the popularity. This forces them to be the ideal types for what we have
  // specified.
  program.ForEachVariable([&] (const GlobalVarDecl *decl) -> bool {
    const auto var = llvm::dyn_cast_or_null<llvm::GlobalVariable>(
        nearby[decl->address]);
    if (!var) {
      LOG(WARNING)
          << "Variable '" << decl->name << "' at address " << std::hex
          << decl->address << std::dec << " shadows a function";
      return true;
    }

    std::vector<llvm::Type *> types;
    FlattenTypeInto(var->getValueType(), types);
    size_t offset = 0;
    for (auto type : types) {

      auto size = dl.getTypeAllocSize(type);
      popularity[decl->address + offset][type] = (~0u >> 1u);
      offset += size;
    }

    // Keep track of the maximum declared size of a global variable. We'll use
    // it to decide how far to look forward/backward given an arbitrary
    // address.
    if (offset > max_var_size) {
      max_var_size = offset;
    }

    return true;
  });

  program.ForEachFunction([&] (const FunctionDecl *decl) {
    const auto func = llvm::dyn_cast_or_null<llvm::Function>(
        nearby[decl->address]);
    if (!func) {
      return true;
    }

    popularity[decl->address][func->getType()] = ~0u;
    return true;
  });

  std::sort(global_cells.begin(), global_cells.end(), order_cells);

  // Try to partition what we know about memory into global variables, and then
  // add them to `nearby` as new globals.
  max_var_size = DeclareMissingGlobals(
      module, global_cells, nearby, max_var_size);

  if (max_var_size) {
    RecoverGlobalVariableAccesses(
        dl, global_cells, nearby, max_var_size);
  }

  // Go find all pointers, so that we can handle displacements from those
  // uniformly. We introduce pointers in terms of parameters, return values,
  // and globals.
  for (auto &global_or_func : nearby) {
    auto global = llvm::dyn_cast<llvm::GlobalVariable>(global_or_func.second);
    if (global) {
      pointers.insert(global);
    } else {
      auto func = llvm::dyn_cast<llvm::Function>(global_or_func.second);
      for (auto &arg : func->args()) {
        if (arg.getType()->isPointerTy()) {
          pointers.insert(&arg);
        }
      }

      if (!func->getReturnType()->isPointerTy()) {
        continue;
      }

      for (auto maybe_caller : func->users()) {
        if (auto ci = llvm::dyn_cast<llvm::CallInst>(maybe_caller)) {
          pointers.insert(ci);
        }
      }
    }
  }

  std::vector<llvm::Instruction *> to_remove;
  auto clear_old_vals = [&to_remove] (void) {
    for (auto inst : to_remove) {
      if (inst->hasNUses(0)) {
        inst->eraseFromParent();
      }
    }
    to_remove.clear();
  };

  // Go through and replace things like `ptrtoint+add` with `gep`s.
  for (auto ptr : pointers) {
    TransformPattern_PTI_Add(dl, ptr, to_remove);
  }

  clear_old_vals();
  pointers.clear();
  LowerMemOps(module, pointers);

  for (auto ptr : pointers) {
    TransformPattern_PTI_Add(dl, ptr, to_remove);
  }

  clear_old_vals();

  auto ptr_size_bits = dl.getPointerSizeInBits(0);

  std::vector<llvm::PHINode *> phi_nodes;
  for (auto changed = true; changed; ) {
    changed = false;

    phi_nodes.clear();
    for (auto &func : module) {
      for (auto &block : func) {
        for (auto &inst : block) {
          if (auto phi = llvm::dyn_cast<llvm::PHINode>(&inst)) {
            if (phi->getType()->isIntegerTy(ptr_size_bits) &&
                phi->hasNUsesOrMore(1)) {
              phi_nodes.push_back(phi);
            }
          }
        }
      }
    }

    pointers.clear();
    for (auto phi : phi_nodes) {
      if (TransformPattern_IntToPtr_PHI(phi, pointers, to_remove)) {
        changed = true;
      }
    }

    for (auto ptr : pointers) {
      if (TransformPattern_PTI_Add(dl, ptr, to_remove)) {
        changed = true;
      }
    }

    clear_old_vals();
  }
}

}  // namespace anvill
