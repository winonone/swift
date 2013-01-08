//===--- TypeInfo.cpp - Type information relevant to SILGen -----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "SILGen.h"
#include "TypeInfo.h"
#include "TypeVisitor.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Types.h"

namespace swift {
namespace Lowering {

static bool isAddressOnly(Type t) {
  // Archetypes and existentials are always address-only.
  // FIXME: Class archetypes and existentials will one day be representable as
  // reference types.
  // FIXME: resilient structs
  return t->is<ArchetypeType>() || t->isExistentialType();
}

namespace {
  enum Loadable_t { IsAddressOnly, IsLoadable };
}
  
/// LoadableTypeInfoVisitor - Recursively descend into fragile struct and
/// tuple types and visit their element types, storing information about the
/// reference type members in the TypeInfo for the type.
class LoadableTypeInfoVisitor : public TypeVisitor<LoadableTypeInfoVisitor,
                                                   Loadable_t>
{
  TypeInfo &theInfo;
  ReferenceTypeElement currentElement;
public:
  LoadableTypeInfoVisitor(TypeInfo &theInfo) : theInfo(theInfo) {}
  
  void pushPath() { currentElement.path.push_back({Type(), 0}); }
  void popPath() { currentElement.path.pop_back(); }
  void setPathType(Type t) { currentElement.path.back().type = t; }
  void advancePath() { ++currentElement.path.back().index; }
  
  Loadable_t visitType(TypeBase *t) {
    if (t->hasReferenceSemantics()) {
      theInfo.referenceTypeElements.push_back(currentElement);
      return IsLoadable;
    }
    if (isAddressOnly(t))
      return IsAddressOnly;
    return IsLoadable;
  }
  
  Loadable_t walkStructDecl(StructDecl *sd) {
    // FIXME: if this struct has a resilient attribute, mark the TypeInfo
    // as addressOnly and bail without checking fields.
    
    pushPath();
    for (Decl *d : sd->getMembers())
      if (VarDecl *vd = dyn_cast<VarDecl>(d))
        if (!vd->isProperty()) {
          CanType ct = vd->getType()->getCanonicalType();
          setPathType(ct);
          if (visit(ct) == IsAddressOnly) {
            return IsAddressOnly;
          }
          advancePath();
        }
    popPath();
    return IsLoadable;
  }
  
  Loadable_t visitBoundGenericType(BoundGenericType *gt) {
    if (StructDecl *sd = dyn_cast<StructDecl>(gt->getDecl()))
      return walkStructDecl(sd);
    else
      return this->visitType(gt);
  }
  
  Loadable_t visitNominalType(NominalType *t) {
    if (StructDecl *sd = dyn_cast<StructDecl>(t->getDecl()))
      return walkStructDecl(sd);
    else
      return this->visitType(t);
  }
  
  Loadable_t visitTupleType(TupleType *t) {
    pushPath();
    for (TupleTypeElt const &elt : t->getFields()) {
      CanType ct = elt.getType()->getCanonicalType();
      setPathType(ct);
      
      visit(ct);
      // FIXME: Address-only tuples complicate function argument handling a bit.
      // Consider all tuples loadable for now, even though this is broken.
      /*
      if (visit(ct) == IsAddressOnly) {
        return IsAddressOnly;
      }
       */
      advancePath();
    }
    popPath();
    return IsLoadable;
  }
};
  
TypeConverter::TypeConverter(SILGenModule &sgm)
  : Context(sgm.M.getContext()) {
}

TypeConverter::~TypeConverter() {
  // The bump pointer allocator destructor will deallocate but not destroy all
  // our TypeInfos.
  for (auto &ti : types) {
    ti.second->~TypeInfo();
  }
}
  
void TypeConverter::makeFragileElementsForDecl(TypeInfo &theInfo,
                                               unsigned &elementIndex,
                                               NominalTypeDecl *decl) {
  if (ClassDecl *cd = dyn_cast<ClassDecl>(decl)) {
    if (cd->hasBaseClass()) {
      makeFragileElementsForDecl(theInfo, elementIndex,
                             cd->getBaseClass()->getClassOrBoundGenericClass());
    }
  }
  
  for (Decl *d : decl->getMembers()) {
    if (VarDecl *vd = dyn_cast<VarDecl>(d)) {
      if (!vd->isProperty()) {
        theInfo.fragileElements[vd] = {vd->getType(),
                                       elementIndex};
        ++elementIndex;
      }
    }
  }
}

void TypeConverter::makeFragileElements(TypeInfo &theInfo, CanType t) {
  // FIXME: check resilient attribute
  unsigned elementIndex = 0;
  if (NominalType *nt = t->getAs<NominalType>()) {
    makeFragileElementsForDecl(theInfo, elementIndex, nt->getDecl());
  } else if (BoundGenericType *bgt = t->getAs<BoundGenericType>()) {
    makeFragileElementsForDecl(theInfo, elementIndex, bgt->getDecl());
  }
}
  
TypeInfo const &TypeConverter::makeTypeInfo(CanType t) {
  void *infoBuffer = TypeInfoBPA.Allocate<TypeInfo>();
  TypeInfo *theInfo = ::new (infoBuffer) TypeInfo();
  types[t.getPointer()] = theInfo;
  bool address = false;
  bool addressOnly = false;
  
  if (LValueType *lvt = t->getAs<LValueType>()) {
    t = lvt->getObjectType()->getCanonicalType();
    address = true;
    addressOnly = getTypeInfo(t).isAddressOnly();
  } else if (t->hasReferenceSemantics()) {
    // Reference types are always loadable, and need only to retain/release
    // themselves.
    addressOnly = false;
    theInfo->referenceTypeElements.push_back(ReferenceTypeElement());
  } else if (isAddressOnly(t)) {
    addressOnly = true;
  } else {
    // walk aggregate types to determine address-only-ness and find reference
    // type elements.
    addressOnly =
      LoadableTypeInfoVisitor(*theInfo).visit(t) == IsAddressOnly;
    if (addressOnly)
      theInfo->referenceTypeElements.clear();
  }
  // If this is a struct or class type, find its fragile elements.
  makeFragileElements(*theInfo, t);
  
  // Generate the lowered type.
  theInfo->loweredType = SILType(t,
                                 /*address=*/address || addressOnly,
                                 /*loadable=*/!addressOnly);
  return *theInfo;
}
  
TypeInfo const &TypeConverter::getTypeInfo(Type t) {
  CanType ct = t->getCanonicalType();
  auto existing = types.find(ct.getPointer());
  if (existing == types.end()) {
    return makeTypeInfo(ct);
  } else
    return *existing->second;
}
  
SILType TypeConverter::getConstantType(SILConstant constant) {
  auto found = constantTypes.find(constant);
  if (found == constantTypes.end()) {
    Type swiftTy = makeConstantType(constant);
    SILType loweredTy = getTypeInfo(swiftTy).getLoweredType();
    constantTypes[constant] = loweredTy;
    return loweredTy;
  } else
    return found->second;
}

/// Get the type of a property accessor, () -> T for a getter or (value:T) -> ()
/// for a setter.
Type TypeConverter::getPropertyType(unsigned kind, Type valueType) const {
  if (kind == SILConstant::Getter) {
    return FunctionType::get(TupleType::getEmpty(Context), valueType, Context);
  }
  if (kind == SILConstant::Setter) {
    TupleTypeElt valueParam(valueType, Context.getIdentifier("value"));
    return FunctionType::get(TupleType::get(valueParam, Context),
                             TupleType::getEmpty(Context),
                             Context);
  }
  llvm_unreachable("not a property constant");
}

/// Get the type of a subscript accessor, Index -> PropertyAccessor.
Type TypeConverter::getSubscriptPropertyType(unsigned kind,
                                             Type indexType,
                                             Type elementType) const {
  Type propertyType = getPropertyType(kind, elementType);
  return FunctionType::get(indexType, propertyType, Context);
}

/// Get the type of the 'this' parameter for methods of a type.
Type TypeConverter::getMethodThisType(Type thisType) const {
  if (thisType->hasReferenceSemantics()) {
    return thisType;
  } else {
    return LValueType::get(thisType, LValueType::Qual::DefaultForType, Context);
  }
}

Type TypeConverter::getMethodTypeInContext(Type /*nullable*/ contextType,
                                       Type methodType,
                                       GenericParamList *genericParams) const {
  if (!contextType)
    return methodType;
  Type thisType = getMethodThisType(contextType);
  
  if (genericParams) {
    return PolymorphicFunctionType::get(thisType, methodType,
                                        genericParams,
                                        Context);
  }

  return FunctionType::get(thisType, methodType, Context);
}

/// Get the type of a global variable accessor function, () -> [byref] T.
static Type getGlobalAccessorType(Type varType, ASTContext &C) {
  return FunctionType::get(TupleType::getEmpty(C),
                           LValueType::get(varType,
                                           LValueType::Qual::DefaultForType,
                                           C),
                           C);
}

/// Get the type of a destructor function, This -> ().
static Type getDestructorType(ClassDecl *cd, ASTContext &C) {
  Type classType = cd->getDeclaredTypeInContext();

  Type voidType = TupleType::getEmpty(C);
  if (cd->getGenericParams()) {
    return PolymorphicFunctionType::get(classType,
                                        voidType,
                                        cd->getGenericParams(),
                                        C);
  } else {
    return FunctionType::get(classType,
                             voidType,
                             C);
  }
}

// Get the type of a constructor's initializer, This -> ConstructorArgs -> ().
static Type getInitializerType(TypeConverter &tc,
                               ConstructorDecl *cd,
                               ASTContext &C) {
  AnyFunctionType *ctorType = cd->getType()->castTo<AnyFunctionType>();
  // The constructor's main entry point type is
  // This.metatype -> ConstructorArgs -> This. Rearrange it to get the
  // initializer type we want.
  Type voidType = TupleType::getEmpty(C);
  Type argsType = ctorType->getResult()->castTo<FunctionType>()->getInput();
  Type retType = ctorType->getResult()->castTo<FunctionType>()->getResult();
  
  Type methodType = FunctionType::get(argsType, voidType, C);
  Type thisType = tc.getMethodThisType(retType);
  
  if (PolymorphicFunctionType *pCtorType =
        ctorType->getAs<PolymorphicFunctionType>()) {
    return PolymorphicFunctionType::get(thisType,
                                        methodType,
                                        &pCtorType->getGenericParams(),
                                        C);
  } else {
    return FunctionType::get(thisType, methodType, C);
  }
}

Type TypeConverter::makeConstantType(SILConstant c) {
  // TODO: mangle function types for address-only indirect arguments and returns
  if (ValueDecl *vd = c.loc.dyn_cast<ValueDecl*>()) {
    Type /*nullable*/ contextType =
      vd->getDeclContext()->getDeclaredTypeOfContext();
    GenericParamList *genericParams = nullptr;
    if (contextType) {
      if (UnboundGenericType *ugt = contextType->getAs<UnboundGenericType>()) {
        // Bind the generic parameters.
        // FIXME: see computeThisType()
        genericParams = ugt->getDecl()->getGenericParams();
        contextType = vd->getDeclContext()->getDeclaredTypeInContext();
      }
    }
    if (SubscriptDecl *sd = dyn_cast<SubscriptDecl>(vd)) {
      // If this is a subscript accessor, derive the accessor type.
      Type subscriptType = getSubscriptPropertyType(c.getKind(),
                                                    sd->getIndices()->getType(),
                                                    sd->getElementType());
      return getMethodTypeInContext(contextType, subscriptType, genericParams);
    } else {
      // If this is a destructor, derive the destructor type.
      if (c.getKind() == SILConstant::Destructor) {
        return getDestructorType(cast<ClassDecl>(vd), Context);
      }
      
      // If this is a constructor initializer, derive the initializer type.
      if (c.getKind() == SILConstant::Initializer) {
        return getInitializerType(*this, cast<ConstructorDecl>(vd), Context);
      }
      
      // If this is a property accessor, derive the property type.
      if (c.isProperty()) {
        Type propertyType = getPropertyType(c.getKind(), vd->getType());
        return getMethodTypeInContext(contextType, propertyType, genericParams);
      }

      // If it's a global var, derive the initializer/accessor function type
      // () -> [byref] T
      if (VarDecl *var = dyn_cast<VarDecl>(vd)) {
        assert(!var->isProperty() && "constant ref to non-physical global var");
        return getGlobalAccessorType(var->getType(), Context);
      }
      
      // Otherwise, return the Swift-level type.
      return vd->getTypeOfReference();
    }
  } else if (CapturingExpr *e = c.loc.dyn_cast<CapturingExpr*>()) {
    assert(c.getKind() == 0 &&
           "closure constant should not be getter, setter, or dtor");
    return e->getType();
  }
  llvm_unreachable("unexpected constant loc");
}
  
} // namespace Lowering
} // namespace swift
