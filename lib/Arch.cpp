#include <vector>

#include "anvill/Arch.h"
#include "anvill/Decl.h"

#include <glog/logging.h>
#include <remill/Arch/Arch.h>

namespace remill {
class Arch;
class IntrinsicTable;
struct Register;
}  // namespace remill

namespace anvill {

// Try to recover parameter names using debug information. Otherwise, name the
// parameters with the form "param_x". The mapping of the return value is
// positional starting at 1.
std::map<unsigned, std::string> TryRecoverParamNames(
    const llvm::Function &function) {
  std::map<unsigned int, std::string> param_names;

  for (auto &block : function) {
    for (auto &inst : block) {
      if (auto debug_inst = llvm::dyn_cast<llvm::DbgInfoIntrinsic>(&inst)) {
        if (auto value_intrin = llvm::dyn_cast<llvm::DbgDeclareInst>(&inst)) {
          const llvm::MDNode *mdn = value_intrin->getVariable();
          const llvm::DILocalVariable *div =
              llvm::cast<llvm::DILocalVariable>(mdn);

          // Make sure it is actually an argument
          if (div->getArg() != 0) {
            LOG(INFO) << div->getArg() << " : " << div->getName().data();
            param_names[div->getArg()] = div->getName().data();
          }
        } else if (auto value_intrin =
                       llvm::dyn_cast<llvm::DbgValueInst>(debug_inst)) {
          const llvm::MDNode *mdn = value_intrin->getVariable();
          const llvm::DILocalVariable *div =
              llvm::cast<llvm::DILocalVariable>(mdn);

          if (div->getArg() != 0) {
            LOG(INFO) << div->getArg() << " : " << div->getName().data();
            param_names[div->getArg()] = div->getName().data();
          }
        }
      }
    }
  }

  // If we don't have names for some parameters then automatically name them
  unsigned int num_args =
      (unsigned int)(function.args().end() - function.args().begin());
  for (unsigned int i = 1; i <= num_args; i++) {
    if (!param_names.count(i)) {
      param_names[i] = "param" + std::to_string(i);
    }
  }

  return param_names;
}

// Try to allocate a register for the argument based on the register constraints
// and what has already been reserved. Return nullptr if there is no possible
// register allocation.
remill::Register *TryRegisterAllocate(
    llvm::Type &type, std::vector<bool> &reserved,
    const std::vector<RegisterConstraint> &register_constraints) {
  SizeConstraint size_constraint;
  TypeConstraint type_constraint;

  switch (type.getTypeID()) {
    case llvm::Type::IntegerTyID: {
      type_constraint = kTypeInt;
      auto derived = llvm::cast<llvm::IntegerType>(type);
      unsigned int width = derived.getBitWidth();
      if (width == 64) {
        size_constraint = kMinBit64;
      } else {
        // TODO(aty): I know that this is wrong but for now its fine
        size_constraint = kMinBit32;
      }
      break;
    }
    case llvm::Type::FloatTyID: {
      type_constraint = kTypeFloat;
      // We automatically know it is 32-bit IEEE floating point type
      size_constraint = kMinBit32;
      break;
    }
    case llvm::Type::DoubleTyID: {
      type_constraint = kTypeFloat;
      // We automatically know it is 64-bit IEEE floating point type
      size_constraint = kMinBit64;
      break;
    }
    case llvm::Type::PointerTyID: {
      type_constraint = kTypeIntegral;
      size_constraint = kMinBit64;
      break;
    }
    case llvm::Type::X86_FP80TyID: {
      type_constraint = kTypeIntegral;
      size_constraint = kMinBit80;
    }
    default: {
      LOG(FATAL) << "Could not assign type and size constraints in "
                    "TryRegisterAllocate()";
      // TODO(aty): Handle other types like X86_MMXTyID, etc.
      break;
    }
  }

  for (size_t i = 0; i < register_constraints.size(); i++) {
    if (reserved[i]) continue;

    const RegisterConstraint &constraint = register_constraints[i];
    // Iterate through the different sizes of a single register to find the
    // smallest possible match
    for (auto const &variant : constraint.variants) {
      if (size_constraint & variant.size_constraint &&
          type_constraint & variant.type_constraint) {
        reserved[i] = true;
        remill::Register *reg =
            new remill::Register(variant.register_name, 0, 0, 0, &type);
        return reg;
      }
    }
  }
  return nullptr;
}

// For each element of the struct, try to allocate it to a register, if all of
// them can be allocated, then return that allocation. Otherwise return a
// nullptr.
std::unique_ptr<std::vector<anvill::ValueDecl>> TryReturnThroughRegisters(
    const llvm::StructType &st,
    const std::vector<RegisterConstraint> &constraints) {
  auto ret = std::make_unique<std::vector<anvill::ValueDecl>>();
  std::vector<bool> reserved(constraints.size(), false);
  for (unsigned i = 0; i < st.getNumElements(); i++) {
    anvill::ValueDecl value_decl = {};
    auto reg =
        TryRegisterAllocate(*st.getElementType(i), reserved, constraints);
    if (reg) {
      value_decl.reg = reg;
      value_decl.type = st.getElementType(i);
    } else {
      // The struct cannot be split over registers
      return nullptr;
    }
    ret->push_back(value_decl);
  }

  return ret;
}

std::vector<anvill::ValueDecl> X86_64_SysV::BindReturnValues(
    const llvm::Function &function) {
  std::vector<anvill::ValueDecl> return_value_declarations;
  anvill::ValueDecl value_declaration = {};
  llvm::Type *ret_type = function.getReturnType();
  value_declaration.type = ret_type;

  switch (ret_type->getTypeID()) {
    case llvm::Type::IntegerTyID:
    case llvm::Type::PointerTyID: {
      // Allocate RAX for an integer or pointer
      value_declaration.reg =
          new remill::Register("RAX", 0, 8, 0, value_declaration.type);
      break;
    }
    case llvm::Type::FloatTyID: {
      // Allocate XMM0 for a floating point value
      value_declaration.reg =
          new remill::Register("XMM0", 0, 16, 0, value_declaration.type);
      break;
    }
    case llvm::Type::StructTyID: {
      // Try to split the struct over the registers
      std::vector<bool> allocated(return_register_constraints.size(), false);
      auto struct_ptr = llvm::cast<llvm::StructType>(value_declaration.type);
      auto mapping =
          TryReturnThroughRegisters(*struct_ptr, return_register_constraints);
      if (mapping) {
        // There is a valid split over registers, so add the mapping
        return *mapping;
      } else {
        // Struct splitting didn't work so do RVO. Assume that the pointer
        // to the return value resides in RAX.
        value_declaration.reg =
            new remill::Register("RAX", 0, 8, 0, value_declaration.type);
      }
      break;
    }
    default: {
      LOG(ERROR) << "Encountered an unknown return type";
      exit(1);
    }
  }

  return_value_declarations.push_back(value_declaration);

  return return_value_declarations;
}

std::vector<anvill::ParameterDecl> X86_64_SysV::BindParameters(
    const llvm::Function &function) {
  std::vector<anvill::ParameterDecl> parameter_declarations;
  auto param_names = TryRecoverParamNames(function);
  llvm::DataLayout dl(function.getParent());

  // Used to keep track of which registers have been allocated
  std::vector<bool> allocated(parameter_register_constraints.size(), false);

  // Stack position of the first argument
  unsigned int stack_offset = 16;

  for (auto &argument : function.args()) {
    anvill::ParameterDecl declaration = {};
    declaration.type = argument.getType();

    // Try to allocate from a register. If a register is not available then
    // allocate from the stack.
    if (remill::Register *reg = TryRegisterAllocate(
            *argument.getType(), allocated, parameter_register_constraints)) {
      declaration.reg = reg;
    } else {
      remill::Register *mem_reg =
          new remill::Register("RSP", stack_offset, 8, 0, argument.getType());

      stack_offset += dl.getTypeAllocSize(argument.getType());
      declaration.mem_reg = mem_reg;
    }

    // Try to get a name for the IR parameter
    // Need to add 1 because param_names uses logical numbering, but
    // argument.getArgNo() uses index numbering
    declaration.name = param_names[argument.getArgNo() + 1];

    parameter_declarations.push_back(declaration);
  }

  return parameter_declarations;
}

remill::Register *X86_64_SysV::BindReturnStackPointer(
    const llvm::Function &function) {
  // For the X86_64_SysV ABI, it is always:
  //
  // "return_stack_pointer": {
  //     "offset": "8",
  //     "register": "RSP",
  //     "type": "L"
  // }

  auto int64_ptr_ty = llvm::IntegerType::get(function.getContext(), 64);
  return new remill::Register("RSP", 8, 8, 0, int64_ptr_ty);
}

std::vector<ParameterDecl> X86_C::BindParameters(
    const llvm::Function &function) {
  std::vector<anvill::ParameterDecl> parameter_declarations;
  auto param_names = TryRecoverParamNames(function);
  llvm::DataLayout dl(function.getParent());

  // Stack position of the first argument
  unsigned int stack_offset = 4;

  for (auto &argument : function.args()) {
    anvill::ParameterDecl declaration = {};
    declaration.type = argument.getType();

    // Since there are no registers, just allocate from the stack
    remill::Register *mem_reg =
        new remill::Register("ESP", stack_offset, 4, 0, argument.getType());

    stack_offset += dl.getTypeAllocSize(argument.getType());
    declaration.mem_reg = mem_reg;

    // Get a name for the IR parameter.
    // Need to add 1 because param_names uses logical numbering, but
    // argument.getArgNo() uses index numbering.
    declaration.name = param_names[argument.getArgNo() + 1];
    parameter_declarations.push_back(declaration);
  }

  return parameter_declarations;
}

std::vector<anvill::ValueDecl> X86_C::BindReturnValues(
    const llvm::Function &function) {
  std::vector<anvill::ValueDecl> return_value_declarations;
  anvill::ValueDecl value_declaration = {};
  llvm::Type *ret_type = function.getReturnType();
  value_declaration.type = ret_type;

  switch (ret_type->getTypeID()) {
    case llvm::Type::IntegerTyID:
    case llvm::Type::PointerTyID: {
      // Allocate EAX for an integer or pointer
      value_declaration.reg =
          new remill::Register("EAX", 0, 8, 0, value_declaration.type);
      break;
    }
    case llvm::Type::FloatTyID: {
      // Allocate ST0 for a floating point value
      value_declaration.reg =
          new remill::Register("ST0", 0, 16, 0, value_declaration.type);
      break;
    }
    case llvm::Type::StructTyID: {
      // Try to split the struct over the registers
      std::vector<bool> allocated(return_register_constraints.size(), false);
      auto struct_ptr = llvm::cast<llvm::StructType>(value_declaration.type);
      auto mapping =
          TryReturnThroughRegisters(*struct_ptr, return_register_constraints);
      if (mapping) {
        // There is a valid split over registers, so return the mapping
        return *mapping;
      } else {
        // Struct splitting didn't work so do RVO. Assume that the pointer
        // to the return value resides in EAX.
        value_declaration.reg =
            new remill::Register("EAX", 0, 8, 0, value_declaration.type);
      }
      break;
    }
    default: {
      LOG(ERROR) << "Encountered an unknown return type";
      exit(1);
    }
  }
  return_value_declarations.push_back(value_declaration);

  return return_value_declarations;
}

remill::Register *X86_C::BindReturnStackPointer(
    const llvm::Function &function) {
  // For X86_C ABI, it is always:
  //
  // "return_stack_pointer": {
  //   "register": "ESP",
  //   "offset": 4
  // }

  auto int32_ptr_ty = llvm::IntegerType::get(function.getContext(), 32);
  return new remill::Register("ESP", 4, 4, 0, int32_ptr_ty);
}

}  // namespace anvill