/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "induction_var_range.h"

#include <limits>

namespace art {

/** Returns true if 64-bit constant fits in 32-bit constant. */
static bool CanLongValueFitIntoInt(int64_t c) {
  return std::numeric_limits<int32_t>::min() <= c && c <= std::numeric_limits<int32_t>::max();
}

/** Returns true if 32-bit addition can be done safely. */
static bool IsSafeAdd(int32_t c1, int32_t c2) {
  return CanLongValueFitIntoInt(static_cast<int64_t>(c1) + static_cast<int64_t>(c2));
}

/** Returns true if 32-bit subtraction can be done safely. */
static bool IsSafeSub(int32_t c1, int32_t c2) {
  return CanLongValueFitIntoInt(static_cast<int64_t>(c1) - static_cast<int64_t>(c2));
}

/** Returns true if 32-bit multiplication can be done safely. */
static bool IsSafeMul(int32_t c1, int32_t c2) {
  return CanLongValueFitIntoInt(static_cast<int64_t>(c1) * static_cast<int64_t>(c2));
}

/** Returns true if 32-bit division can be done safely. */
static bool IsSafeDiv(int32_t c1, int32_t c2) {
  return c2 != 0 && CanLongValueFitIntoInt(static_cast<int64_t>(c1) / static_cast<int64_t>(c2));
}

/** Returns true for 32/64-bit constant instruction. */
static bool IsIntAndGet(HInstruction* instruction, int64_t* value) {
  if (instruction->IsIntConstant()) {
    *value = instruction->AsIntConstant()->GetValue();
    return true;
  } else if (instruction->IsLongConstant()) {
    *value = instruction->AsLongConstant()->GetValue();
    return true;
  }
  return false;
}

/** Returns b^e for b,e >= 1. */
static int64_t IntPow(int64_t b, int64_t e) {
  DCHECK_GE(b, 1);
  DCHECK_GE(e, 1);
  int64_t pow = 1;
  while (e) {
    if (e & 1) {
      pow *= b;
    }
    e >>= 1;
    b *= b;
  }
  return pow;
}

/**
 * Detects an instruction that is >= 0. As long as the value is carried by
 * a single instruction, arithmetic wrap-around cannot occur.
 */
static bool IsGEZero(HInstruction* instruction) {
  DCHECK(instruction != nullptr);
  if (instruction->IsArrayLength()) {
    return true;
  } else if (instruction->IsInvokeStaticOrDirect()) {
    switch (instruction->AsInvoke()->GetIntrinsic()) {
      case Intrinsics::kMathMinIntInt:
      case Intrinsics::kMathMinLongLong:
        // Instruction MIN(>=0, >=0) is >= 0.
        return IsGEZero(instruction->InputAt(0)) &&
               IsGEZero(instruction->InputAt(1));
      case Intrinsics::kMathAbsInt:
      case Intrinsics::kMathAbsLong:
        // Instruction ABS(x) is >= 0.
        return true;
      default:
        break;
    }
  }
  int64_t value = -1;
  return IsIntAndGet(instruction, &value) && value >= 0;
}

/** Hunts "under the hood" for a suitable instruction at the hint. */
static bool IsMaxAtHint(
    HInstruction* instruction, HInstruction* hint, /*out*/HInstruction** suitable) {
  if (instruction->IsInvokeStaticOrDirect()) {
    switch (instruction->AsInvoke()->GetIntrinsic()) {
      case Intrinsics::kMathMinIntInt:
      case Intrinsics::kMathMinLongLong:
        // For MIN(x, y), return most suitable x or y as maximum.
        return IsMaxAtHint(instruction->InputAt(0), hint, suitable) ||
               IsMaxAtHint(instruction->InputAt(1), hint, suitable);
      default:
        break;
    }
  } else {
    *suitable = instruction;
    while (instruction->IsArrayLength() ||
           instruction->IsNullCheck() ||
           instruction->IsNewArray()) {
      instruction = instruction->InputAt(0);
    }
    return instruction == hint;
  }
  return false;
}

/** Post-analysis simplification of a minimum value that makes the bound more useful to clients. */
static InductionVarRange::Value SimplifyMin(InductionVarRange::Value v) {
  if (v.is_known && v.a_constant == 1 && v.b_constant <= 0) {
    // If a == 1,  instruction >= 0 and b <= 0, just return the constant b.
    // No arithmetic wrap-around can occur.
    if (IsGEZero(v.instruction)) {
      return InductionVarRange::Value(v.b_constant);
    }
  }
  return v;
}

/** Post-analysis simplification of a maximum value that makes the bound more useful to clients. */
static InductionVarRange::Value SimplifyMax(InductionVarRange::Value v, HInstruction* hint) {
  if (v.is_known && v.a_constant >= 1) {
    // An upper bound a * (length / a) + b, where a >= 1, can be conservatively rewritten as
    // length + b because length >= 0 is true.
    int64_t value;
    if (v.instruction->IsDiv() &&
        v.instruction->InputAt(0)->IsArrayLength() &&
        IsIntAndGet(v.instruction->InputAt(1), &value) && v.a_constant == value) {
      return InductionVarRange::Value(v.instruction->InputAt(0), 1, v.b_constant);
    }
    // If a == 1, the most suitable one suffices as maximum value.
    HInstruction* suitable = nullptr;
    if (v.a_constant == 1 && IsMaxAtHint(v.instruction, hint, &suitable)) {
      return InductionVarRange::Value(suitable, 1, v.b_constant);
    }
  }
  return v;
}

/** Tests for a constant value. */
static bool IsConstantValue(InductionVarRange::Value v) {
  return v.is_known && v.a_constant == 0;
}

/** Corrects a value for type to account for arithmetic wrap-around in lower precision. */
static InductionVarRange::Value CorrectForType(InductionVarRange::Value v, Primitive::Type type) {
  switch (type) {
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimByte: {
      // Constants within range only.
      // TODO: maybe some room for improvement, like allowing widening conversions
      const int32_t min = Primitive::MinValueOfIntegralType(type);
      const int32_t max = Primitive::MaxValueOfIntegralType(type);
      return (IsConstantValue(v) && min <= v.b_constant && v.b_constant <= max)
          ? v
          : InductionVarRange::Value();
    }
    default:
      return v;
  }
}

/** Inserts an instruction. */
static HInstruction* Insert(HBasicBlock* block, HInstruction* instruction) {
  DCHECK(block != nullptr);
  DCHECK(block->GetLastInstruction() != nullptr) << block->GetBlockId();
  DCHECK(instruction != nullptr);
  block->InsertInstructionBefore(instruction, block->GetLastInstruction());
  return instruction;
}

/** Obtains loop's control instruction. */
static HInstruction* GetLoopControl(HLoopInformation* loop) {
  DCHECK(loop != nullptr);
  return loop->GetHeader()->GetLastInstruction();
}

//
// Public class methods.
//

InductionVarRange::InductionVarRange(HInductionVarAnalysis* induction_analysis)
    : induction_analysis_(induction_analysis),
      chase_hint_(nullptr) {
  DCHECK(induction_analysis != nullptr);
}

bool InductionVarRange::GetInductionRange(HInstruction* context,
                                          HInstruction* instruction,
                                          HInstruction* chase_hint,
                                          /*out*/Value* min_val,
                                          /*out*/Value* max_val,
                                          /*out*/bool* needs_finite_test) {
  HLoopInformation* loop = nullptr;
  HInductionVarAnalysis::InductionInfo* info = nullptr;
  HInductionVarAnalysis::InductionInfo* trip = nullptr;
  if (!HasInductionInfo(context, instruction, &loop, &info, &trip)) {
    return false;
  }
  // Type int or lower (this is not too restrictive since intended clients, like
  // bounds check elimination, will have truncated higher precision induction
  // at their use point already).
  switch (info->type) {
    case Primitive::kPrimInt:
    case Primitive::kPrimShort:
    case Primitive::kPrimChar:
    case Primitive::kPrimByte:
      break;
    default:
      return false;
  }
  // Find range.
  chase_hint_ = chase_hint;
  bool in_body = context->GetBlock() != loop->GetHeader();
  int64_t stride_value = 0;
  *min_val = SimplifyMin(GetVal(info, trip, in_body, /* is_min */ true));
  *max_val = SimplifyMax(GetVal(info, trip, in_body, /* is_min */ false), chase_hint);
  *needs_finite_test = NeedsTripCount(info, &stride_value) && IsUnsafeTripCount(trip);
  chase_hint_ = nullptr;
  // Retry chasing constants for wrap-around (merge sensitive).
  if (!min_val->is_known && info->induction_class == HInductionVarAnalysis::kWrapAround) {
    *min_val = SimplifyMin(GetVal(info, trip, in_body, /* is_min */ true));
  }
  return true;
}

bool InductionVarRange::CanGenerateRange(HInstruction* context,
                                         HInstruction* instruction,
                                         /*out*/bool* needs_finite_test,
                                         /*out*/bool* needs_taken_test) {
  bool is_last_value = false;
  int64_t stride_value = 0;
  return GenerateRangeOrLastValue(context,
                                  instruction,
                                  is_last_value,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,  // nothing generated yet
                                  &stride_value,
                                  needs_finite_test,
                                  needs_taken_test)
      && (stride_value == -1 ||
          stride_value == 0 ||
          stride_value == 1);  // avoid arithmetic wrap-around anomalies.
}

void InductionVarRange::GenerateRange(HInstruction* context,
                                      HInstruction* instruction,
                                      HGraph* graph,
                                      HBasicBlock* block,
                                      /*out*/HInstruction** lower,
                                      /*out*/HInstruction** upper) {
  bool is_last_value = false;
  int64_t stride_value = 0;
  bool b1, b2;  // unused
  if (!GenerateRangeOrLastValue(context,
                                instruction,
                                is_last_value,
                                graph,
                                block,
                                lower,
                                upper,
                                nullptr,
                                &stride_value,
                                &b1,
                                &b2)) {
    LOG(FATAL) << "Failed precondition: CanGenerateRange()";
  }
}

HInstruction* InductionVarRange::GenerateTakenTest(HInstruction* context,
                                                   HGraph* graph,
                                                   HBasicBlock* block) {
  HInstruction* taken_test = nullptr;
  bool is_last_value = false;
  int64_t stride_value = 0;
  bool b1, b2;  // unused
  if (!GenerateRangeOrLastValue(context,
                                context,
                                is_last_value,
                                graph,
                                block,
                                nullptr,
                                nullptr,
                                &taken_test,
                                &stride_value,
                                &b1,
                                &b2)) {
    LOG(FATAL) << "Failed precondition: CanGenerateRange()";
  }
  return taken_test;
}

bool InductionVarRange::CanGenerateLastValue(HInstruction* instruction) {
  bool is_last_value = true;
  int64_t stride_value = 0;
  bool needs_finite_test = false;
  bool needs_taken_test = false;
  return GenerateRangeOrLastValue(instruction,
                                  instruction,
                                  is_last_value,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,
                                  nullptr,  // nothing generated yet
                                  &stride_value,
                                  &needs_finite_test,
                                  &needs_taken_test)
      && !needs_finite_test && !needs_taken_test;
}

HInstruction* InductionVarRange::GenerateLastValue(HInstruction* instruction,
                                                   HGraph* graph,
                                                   HBasicBlock* block) {
  HInstruction* last_value = nullptr;
  bool is_last_value = true;
  int64_t stride_value = 0;
  bool b1, b2;  // unused
  if (!GenerateRangeOrLastValue(instruction,
                                instruction,
                                is_last_value,
                                graph,
                                block,
                                &last_value,
                                &last_value,
                                nullptr,
                                &stride_value,
                                &b1,
                                &b2)) {
    LOG(FATAL) << "Failed precondition: CanGenerateLastValue()";
  }
  return last_value;
}

void InductionVarRange::Replace(HInstruction* instruction,
                                HInstruction* fetch,
                                HInstruction* replacement) {
  for (HLoopInformation* lp = instruction->GetBlock()->GetLoopInformation();  // closest enveloping loop
       lp != nullptr;
       lp = lp->GetPreHeader()->GetLoopInformation()) {
    // Update instruction's information.
    ReplaceInduction(induction_analysis_->LookupInfo(lp, instruction), fetch, replacement);
    // Update loop's trip-count information.
    ReplaceInduction(induction_analysis_->LookupInfo(lp, GetLoopControl(lp)), fetch, replacement);
  }
}

bool InductionVarRange::IsFinite(HLoopInformation* loop) const {
  HInductionVarAnalysis::InductionInfo *trip =
      induction_analysis_->LookupInfo(loop, GetLoopControl(loop));
  return trip != nullptr && !IsUnsafeTripCount(trip);
}

//
// Private class methods.
//

bool InductionVarRange::IsConstant(HInductionVarAnalysis::InductionInfo* info,
                                   ConstantRequest request,
                                   /*out*/ int64_t* value) const {
  if (info != nullptr) {
    // A direct 32-bit or 64-bit constant fetch. This immediately satisfies
    // any of the three requests (kExact, kAtMost, and KAtLeast).
    if (info->induction_class == HInductionVarAnalysis::kInvariant &&
        info->operation == HInductionVarAnalysis::kFetch) {
      if (IsIntAndGet(info->fetch, value)) {
        return true;
      }
    }
    // Try range analysis on the invariant, only accept a proper range
    // to avoid arithmetic wrap-around anomalies.
    Value min_val = GetVal(info, nullptr, /* in_body */ true, /* is_min */ true);
    Value max_val = GetVal(info, nullptr, /* in_body */ true, /* is_min */ false);
    if (IsConstantValue(min_val) &&
        IsConstantValue(max_val) && min_val.b_constant <= max_val.b_constant) {
      if ((request == kExact && min_val.b_constant == max_val.b_constant) || request == kAtMost) {
        *value = max_val.b_constant;
        return true;
      } else if (request == kAtLeast) {
        *value = min_val.b_constant;
        return true;
      }
    }
  }
  return false;
}

bool InductionVarRange::HasInductionInfo(
    HInstruction* context,
    HInstruction* instruction,
    /*out*/ HLoopInformation** loop,
    /*out*/ HInductionVarAnalysis::InductionInfo** info,
    /*out*/ HInductionVarAnalysis::InductionInfo** trip) const {
  DCHECK(context != nullptr);
  DCHECK(context->GetBlock() != nullptr);
  HLoopInformation* lp = context->GetBlock()->GetLoopInformation();  // closest enveloping loop
  if (lp != nullptr) {
    HInductionVarAnalysis::InductionInfo* i = induction_analysis_->LookupInfo(lp, instruction);
    if (i != nullptr) {
      *loop = lp;
      *info = i;
      *trip = induction_analysis_->LookupInfo(lp, GetLoopControl(lp));
      return true;
    }
  }
  return false;
}

bool InductionVarRange::IsWellBehavedTripCount(HInductionVarAnalysis::InductionInfo* trip) const {
  if (trip != nullptr) {
    // Both bounds that define a trip-count are well-behaved if they either are not defined
    // in any loop, or are contained in a proper interval. This allows finding the min/max
    // of an expression by chasing outward.
    InductionVarRange range(induction_analysis_);
    HInductionVarAnalysis::InductionInfo* lower = trip->op_b->op_a;
    HInductionVarAnalysis::InductionInfo* upper = trip->op_b->op_b;
    int64_t not_used = 0;
    return (!HasFetchInLoop(lower) || range.IsConstant(lower, kAtLeast, &not_used)) &&
           (!HasFetchInLoop(upper) || range.IsConstant(upper, kAtLeast, &not_used));
  }
  return true;
}

bool InductionVarRange::HasFetchInLoop(HInductionVarAnalysis::InductionInfo* info) const {
  if (info != nullptr) {
    if (info->induction_class == HInductionVarAnalysis::kInvariant &&
        info->operation == HInductionVarAnalysis::kFetch) {
      return info->fetch->GetBlock()->GetLoopInformation() != nullptr;
    }
    return HasFetchInLoop(info->op_a) || HasFetchInLoop(info->op_b);
  }
  return false;
}

bool InductionVarRange::NeedsTripCount(HInductionVarAnalysis::InductionInfo* info,
                                       int64_t* stride_value) const {
  if (info != nullptr) {
    if (info->induction_class == HInductionVarAnalysis::kLinear) {
      return IsConstant(info->op_a, kExact, stride_value);
    } else if (info->induction_class == HInductionVarAnalysis::kPolynomial) {
      return NeedsTripCount(info->op_a, stride_value);
    } else if (info->induction_class == HInductionVarAnalysis::kWrapAround) {
      return NeedsTripCount(info->op_b, stride_value);
    }
  }
  return false;
}

bool InductionVarRange::IsBodyTripCount(HInductionVarAnalysis::InductionInfo* trip) const {
  if (trip != nullptr) {
    if (trip->induction_class == HInductionVarAnalysis::kInvariant) {
      return trip->operation == HInductionVarAnalysis::kTripCountInBody ||
             trip->operation == HInductionVarAnalysis::kTripCountInBodyUnsafe;
    }
  }
  return false;
}

bool InductionVarRange::IsUnsafeTripCount(HInductionVarAnalysis::InductionInfo* trip) const {
  if (trip != nullptr) {
    if (trip->induction_class == HInductionVarAnalysis::kInvariant) {
      return trip->operation == HInductionVarAnalysis::kTripCountInBodyUnsafe ||
             trip->operation == HInductionVarAnalysis::kTripCountInLoopUnsafe;
    }
  }
  return false;
}

InductionVarRange::Value InductionVarRange::GetLinear(HInductionVarAnalysis::InductionInfo* info,
                                                      HInductionVarAnalysis::InductionInfo* trip,
                                                      bool in_body,
                                                      bool is_min) const {
  DCHECK(info != nullptr);
  DCHECK_EQ(info->induction_class, HInductionVarAnalysis::kLinear);
  // Detect common situation where an offset inside the trip-count cancels out during range
  // analysis (finding max a * (TC - 1) + OFFSET for a == 1 and TC = UPPER - OFFSET or finding
  // min a * (TC - 1) + OFFSET for a == -1 and TC = OFFSET - UPPER) to avoid losing information
  // with intermediate results that only incorporate single instructions.
  if (trip != nullptr) {
    HInductionVarAnalysis::InductionInfo* trip_expr = trip->op_a;
    if (trip_expr->type == info->type && trip_expr->operation == HInductionVarAnalysis::kSub) {
      int64_t stride_value = 0;
      if (IsConstant(info->op_a, kExact, &stride_value)) {
        if (!is_min && stride_value == 1) {
          // Test original trip's negative operand (trip_expr->op_b) against offset of induction.
          if (HInductionVarAnalysis::InductionEqual(trip_expr->op_b, info->op_b)) {
            // Analyze cancelled trip with just the positive operand (trip_expr->op_a).
            HInductionVarAnalysis::InductionInfo cancelled_trip(
                trip->induction_class,
                trip->operation,
                trip_expr->op_a,
                trip->op_b,
                nullptr,
                trip->type);
            return GetVal(&cancelled_trip, trip, in_body, is_min);
          }
        } else if (is_min && stride_value == -1) {
          // Test original trip's positive operand (trip_expr->op_a) against offset of induction.
          if (HInductionVarAnalysis::InductionEqual(trip_expr->op_a, info->op_b)) {
            // Analyze cancelled trip with just the negative operand (trip_expr->op_b).
            HInductionVarAnalysis::InductionInfo neg(
                HInductionVarAnalysis::kInvariant,
                HInductionVarAnalysis::kNeg,
                nullptr,
                trip_expr->op_b,
                nullptr,
                trip->type);
            HInductionVarAnalysis::InductionInfo cancelled_trip(
                trip->induction_class, trip->operation, &neg, trip->op_b, nullptr, trip->type);
            return SubValue(Value(0), GetVal(&cancelled_trip, trip, in_body, !is_min));
          }
        }
      }
    }
  }
  // General rule of linear induction a * i + b, for normalized 0 <= i < TC.
  return AddValue(GetMul(info->op_a, trip, trip, in_body, is_min),
                  GetVal(info->op_b, trip, in_body, is_min));
}

InductionVarRange::Value InductionVarRange::GetPolynomial(HInductionVarAnalysis::InductionInfo* info,
                                                          HInductionVarAnalysis::InductionInfo* trip,
                                                          bool in_body,
                                                          bool is_min) const {
  DCHECK(info != nullptr);
  DCHECK_EQ(info->induction_class, HInductionVarAnalysis::kPolynomial);
  int64_t a = 0;
  int64_t b = 0;
  if (IsConstant(info->op_a->op_a, kExact, &a) && CanLongValueFitIntoInt(a) && a >= 0 &&
      IsConstant(info->op_a->op_b, kExact, &b) && CanLongValueFitIntoInt(b) && b >= 0) {
    // Evaluate bounds on sum_i=0^m-1(a * i + b) + c with a,b >= 0 for known
    // maximum index value m as a * (m * (m-1)) / 2 + b * m + c.
    Value c = GetVal(info->op_b, trip, in_body, is_min);
    if (is_min) {
      return c;
    } else {
      Value m = GetVal(trip, trip, in_body, is_min);
      Value t = DivValue(MulValue(m, SubValue(m, Value(1))), Value(2));
      Value x = MulValue(Value(a), t);
      Value y = MulValue(Value(b), m);
      return AddValue(AddValue(x, y), c);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetGeometric(HInductionVarAnalysis::InductionInfo* info,
                                                         HInductionVarAnalysis::InductionInfo* trip,
                                                         bool in_body,
                                                         bool is_min) const {
  DCHECK(info != nullptr);
  DCHECK_EQ(info->induction_class, HInductionVarAnalysis::kGeometric);
  int64_t a = 0;
  int64_t f = 0;
  if (IsConstant(info->op_a, kExact, &a) &&
      CanLongValueFitIntoInt(a) &&
      IsIntAndGet(info->fetch, &f) && f >= 1) {
    // Conservative bounds on a * f^-i + b with f >= 1 can be computed without
    // trip count. Other forms would require a much more elaborate evaluation.
    const bool is_min_a = a >= 0 ? is_min : !is_min;
    if (info->operation == HInductionVarAnalysis::kDiv) {
      Value b = GetVal(info->op_b, trip, in_body, is_min);
      return is_min_a ? b : AddValue(Value(a), b);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetFetch(HInstruction* instruction,
                                                     HInductionVarAnalysis::InductionInfo* trip,
                                                     bool in_body,
                                                     bool is_min) const {
  // Special case when chasing constants: single instruction that denotes trip count in the
  // loop-body is minimal 1 and maximal, with safe trip-count, max int,
  if (chase_hint_ == nullptr && in_body && trip != nullptr && instruction == trip->op_a->fetch) {
    if (is_min) {
      return Value(1);
    } else if (!instruction->IsConstant() && !IsUnsafeTripCount(trip)) {
      return Value(std::numeric_limits<int32_t>::max());
    }
  }
  // Unless at a constant or hint, chase the instruction a bit deeper into the HIR tree, so that
  // it becomes more likely range analysis will compare the same instructions as terminal nodes.
  int64_t value;
  if (IsIntAndGet(instruction, &value) && CanLongValueFitIntoInt(value)) {
    // Proper constant reveals best information.
    return Value(static_cast<int32_t>(value));
  } else if (instruction == chase_hint_) {
    // At hint, fetch is represented by itself.
    return Value(instruction, 1, 0);
  } else if (instruction->IsAdd()) {
    // Incorporate suitable constants in the chased value.
    if (IsIntAndGet(instruction->InputAt(0), &value) && CanLongValueFitIntoInt(value)) {
      return AddValue(Value(static_cast<int32_t>(value)),
                      GetFetch(instruction->InputAt(1), trip, in_body, is_min));
    } else if (IsIntAndGet(instruction->InputAt(1), &value) && CanLongValueFitIntoInt(value)) {
      return AddValue(GetFetch(instruction->InputAt(0), trip, in_body, is_min),
                      Value(static_cast<int32_t>(value)));
    }
  } else if (instruction->IsArrayLength()) {
    // Exploit length properties when chasing constants or chase into a new array declaration.
    if (chase_hint_ == nullptr) {
      return is_min ? Value(0) : Value(std::numeric_limits<int32_t>::max());
    } else if (instruction->InputAt(0)->IsNewArray()) {
      return GetFetch(instruction->InputAt(0)->InputAt(0), trip, in_body, is_min);
    }
  } else if (instruction->IsTypeConversion()) {
    // Since analysis is 32-bit (or narrower), chase beyond widening along the path.
    if (instruction->AsTypeConversion()->GetInputType() == Primitive::kPrimInt &&
        instruction->AsTypeConversion()->GetResultType() == Primitive::kPrimLong) {
      return GetFetch(instruction->InputAt(0), trip, in_body, is_min);
    }
  }
  // Chase an invariant fetch that is defined by an outer loop if the trip-count used
  // so far is well-behaved in both bounds and the next trip-count is safe.
  // Example:
  //   for (int i = 0; i <= 100; i++)  // safe
  //     for (int j = 0; j <= i; j++)  // well-behaved
  //       j is in range [0, i  ] (if i is chase hint)
  //         or in range [0, 100] (otherwise)
  HLoopInformation* next_loop = nullptr;
  HInductionVarAnalysis::InductionInfo* next_info = nullptr;
  HInductionVarAnalysis::InductionInfo* next_trip = nullptr;
  bool next_in_body = true;  // inner loop is always in body of outer loop
  if (HasInductionInfo(instruction, instruction, &next_loop, &next_info, &next_trip) &&
      IsWellBehavedTripCount(trip) &&
      !IsUnsafeTripCount(next_trip)) {
    return GetVal(next_info, next_trip, next_in_body, is_min);
  }
  // Fetch is represented by itself.
  return Value(instruction, 1, 0);
}

InductionVarRange::Value InductionVarRange::GetVal(HInductionVarAnalysis::InductionInfo* info,
                                                   HInductionVarAnalysis::InductionInfo* trip,
                                                   bool in_body,
                                                   bool is_min) const {
  if (info != nullptr) {
    switch (info->induction_class) {
      case HInductionVarAnalysis::kInvariant:
        // Invariants.
        switch (info->operation) {
          case HInductionVarAnalysis::kAdd:
            return AddValue(GetVal(info->op_a, trip, in_body, is_min),
                            GetVal(info->op_b, trip, in_body, is_min));
          case HInductionVarAnalysis::kSub:  // second reversed!
            return SubValue(GetVal(info->op_a, trip, in_body, is_min),
                            GetVal(info->op_b, trip, in_body, !is_min));
          case HInductionVarAnalysis::kNeg:  // second reversed!
            return SubValue(Value(0),
                            GetVal(info->op_b, trip, in_body, !is_min));
          case HInductionVarAnalysis::kMul:
            return GetMul(info->op_a, info->op_b, trip, in_body, is_min);
          case HInductionVarAnalysis::kDiv:
            return GetDiv(info->op_a, info->op_b, trip, in_body, is_min);
          case HInductionVarAnalysis::kRem:
            return GetRem(info->op_a, info->op_b);
          case HInductionVarAnalysis::kXor:
            return GetXor(info->op_a, info->op_b);
          case HInductionVarAnalysis::kFetch:
            return GetFetch(info->fetch, trip, in_body, is_min);
          case HInductionVarAnalysis::kTripCountInLoop:
          case HInductionVarAnalysis::kTripCountInLoopUnsafe:
            if (!in_body && !is_min) {  // one extra!
              return GetVal(info->op_a, trip, in_body, is_min);
            }
            FALLTHROUGH_INTENDED;
          case HInductionVarAnalysis::kTripCountInBody:
          case HInductionVarAnalysis::kTripCountInBodyUnsafe:
            if (is_min) {
              return Value(0);
            } else if (in_body) {
              return SubValue(GetVal(info->op_a, trip, in_body, is_min), Value(1));
            }
            break;
          default:
            break;
        }
        break;
      case HInductionVarAnalysis::kLinear:
        return CorrectForType(GetLinear(info, trip, in_body, is_min), info->type);
      case HInductionVarAnalysis::kPolynomial:
        return GetPolynomial(info, trip, in_body, is_min);
      case HInductionVarAnalysis::kGeometric:
        return GetGeometric(info, trip, in_body, is_min);
      case HInductionVarAnalysis::kWrapAround:
      case HInductionVarAnalysis::kPeriodic:
        return MergeVal(GetVal(info->op_a, trip, in_body, is_min),
                        GetVal(info->op_b, trip, in_body, is_min), is_min);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetMul(HInductionVarAnalysis::InductionInfo* info1,
                                                   HInductionVarAnalysis::InductionInfo* info2,
                                                   HInductionVarAnalysis::InductionInfo* trip,
                                                   bool in_body,
                                                   bool is_min) const {
  // Constant times range.
  int64_t value = 0;
  if (IsConstant(info1, kExact, &value)) {
    return MulRangeAndConstant(value, info2, trip, in_body, is_min);
  } else if (IsConstant(info2, kExact, &value)) {
    return MulRangeAndConstant(value, info1, trip, in_body, is_min);
  }
  // Interval ranges.
  Value v1_min = GetVal(info1, trip, in_body, /* is_min */ true);
  Value v1_max = GetVal(info1, trip, in_body, /* is_min */ false);
  Value v2_min = GetVal(info2, trip, in_body, /* is_min */ true);
  Value v2_max = GetVal(info2, trip, in_body, /* is_min */ false);
  // Positive range vs. positive or negative range.
  if (IsConstantValue(v1_min) && v1_min.b_constant >= 0) {
    if (IsConstantValue(v2_min) && v2_min.b_constant >= 0) {
      return is_min ? MulValue(v1_min, v2_min) : MulValue(v1_max, v2_max);
    } else if (IsConstantValue(v2_max) && v2_max.b_constant <= 0) {
      return is_min ? MulValue(v1_max, v2_min) : MulValue(v1_min, v2_max);
    }
  }
  // Negative range vs. positive or negative range.
  if (IsConstantValue(v1_max) && v1_max.b_constant <= 0) {
    if (IsConstantValue(v2_min) && v2_min.b_constant >= 0) {
      return is_min ? MulValue(v1_min, v2_max) : MulValue(v1_max, v2_min);
    } else if (IsConstantValue(v2_max) && v2_max.b_constant <= 0) {
      return is_min ? MulValue(v1_max, v2_max) : MulValue(v1_min, v2_min);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetDiv(HInductionVarAnalysis::InductionInfo* info1,
                                                   HInductionVarAnalysis::InductionInfo* info2,
                                                   HInductionVarAnalysis::InductionInfo* trip,
                                                   bool in_body,
                                                   bool is_min) const {
  // Range divided by constant.
  int64_t value = 0;
  if (IsConstant(info2, kExact, &value)) {
    return DivRangeAndConstant(value, info1, trip, in_body, is_min);
  }
  // Interval ranges.
  Value v1_min = GetVal(info1, trip, in_body, /* is_min */ true);
  Value v1_max = GetVal(info1, trip, in_body, /* is_min */ false);
  Value v2_min = GetVal(info2, trip, in_body, /* is_min */ true);
  Value v2_max = GetVal(info2, trip, in_body, /* is_min */ false);
  // Positive range vs. positive or negative range.
  if (IsConstantValue(v1_min) && v1_min.b_constant >= 0) {
    if (IsConstantValue(v2_min) && v2_min.b_constant >= 0) {
      return is_min ? DivValue(v1_min, v2_max) : DivValue(v1_max, v2_min);
    } else if (IsConstantValue(v2_max) && v2_max.b_constant <= 0) {
      return is_min ? DivValue(v1_max, v2_max) : DivValue(v1_min, v2_min);
    }
  }
  // Negative range vs. positive or negative range.
  if (IsConstantValue(v1_max) && v1_max.b_constant <= 0) {
    if (IsConstantValue(v2_min) && v2_min.b_constant >= 0) {
      return is_min ? DivValue(v1_min, v2_min) : DivValue(v1_max, v2_max);
    } else if (IsConstantValue(v2_max) && v2_max.b_constant <= 0) {
      return is_min ? DivValue(v1_max, v2_min) : DivValue(v1_min, v2_max);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetRem(
    HInductionVarAnalysis::InductionInfo* info1,
    HInductionVarAnalysis::InductionInfo* info2) const {
  int64_t v1 = 0;
  int64_t v2 = 0;
  // Only accept exact values.
  if (IsConstant(info1, kExact, &v1) && IsConstant(info2, kExact, &v2) && v2 != 0) {
    int64_t value = v1 % v2;
    if (CanLongValueFitIntoInt(value)) {
      return Value(static_cast<int32_t>(value));
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::GetXor(
    HInductionVarAnalysis::InductionInfo* info1,
    HInductionVarAnalysis::InductionInfo* info2) const {
  int64_t v1 = 0;
  int64_t v2 = 0;
  // Only accept exact values.
  if (IsConstant(info1, kExact, &v1) && IsConstant(info2, kExact, &v2)) {
    int64_t value = v1 ^ v2;
    if (CanLongValueFitIntoInt(value)) {
      return Value(static_cast<int32_t>(value));
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::MulRangeAndConstant(
    int64_t value,
    HInductionVarAnalysis::InductionInfo* info,
    HInductionVarAnalysis::InductionInfo* trip,
    bool in_body,
    bool is_min) const {
  if (CanLongValueFitIntoInt(value)) {
    Value c(static_cast<int32_t>(value));
    return MulValue(GetVal(info, trip, in_body, is_min == value >= 0), c);
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::DivRangeAndConstant(
    int64_t value,
    HInductionVarAnalysis::InductionInfo* info,
    HInductionVarAnalysis::InductionInfo* trip,
    bool in_body,
    bool is_min) const {
  if (CanLongValueFitIntoInt(value)) {
    Value c(static_cast<int32_t>(value));
    return DivValue(GetVal(info, trip, in_body, is_min == value >= 0), c);
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::AddValue(Value v1, Value v2) const {
  if (v1.is_known && v2.is_known && IsSafeAdd(v1.b_constant, v2.b_constant)) {
    const int32_t b = v1.b_constant + v2.b_constant;
    if (v1.a_constant == 0) {
      return Value(v2.instruction, v2.a_constant, b);
    } else if (v2.a_constant == 0) {
      return Value(v1.instruction, v1.a_constant, b);
    } else if (v1.instruction == v2.instruction && IsSafeAdd(v1.a_constant, v2.a_constant)) {
      return Value(v1.instruction, v1.a_constant + v2.a_constant, b);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::SubValue(Value v1, Value v2) const {
  if (v1.is_known && v2.is_known && IsSafeSub(v1.b_constant, v2.b_constant)) {
    const int32_t b = v1.b_constant - v2.b_constant;
    if (v1.a_constant == 0 && IsSafeSub(0, v2.a_constant)) {
      return Value(v2.instruction, -v2.a_constant, b);
    } else if (v2.a_constant == 0) {
      return Value(v1.instruction, v1.a_constant, b);
    } else if (v1.instruction == v2.instruction && IsSafeSub(v1.a_constant, v2.a_constant)) {
      return Value(v1.instruction, v1.a_constant - v2.a_constant, b);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::MulValue(Value v1, Value v2) const {
  if (v1.is_known && v2.is_known) {
    if (v1.a_constant == 0) {
      if (IsSafeMul(v1.b_constant, v2.a_constant) && IsSafeMul(v1.b_constant, v2.b_constant)) {
        return Value(v2.instruction, v1.b_constant * v2.a_constant, v1.b_constant * v2.b_constant);
      }
    } else if (v2.a_constant == 0) {
      if (IsSafeMul(v1.a_constant, v2.b_constant) && IsSafeMul(v1.b_constant, v2.b_constant)) {
        return Value(v1.instruction, v1.a_constant * v2.b_constant, v1.b_constant * v2.b_constant);
      }
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::DivValue(Value v1, Value v2) const {
  if (v1.is_known && v2.is_known && v1.a_constant == 0 && v2.a_constant == 0) {
    if (IsSafeDiv(v1.b_constant, v2.b_constant)) {
      return Value(v1.b_constant / v2.b_constant);
    }
  }
  return Value();
}

InductionVarRange::Value InductionVarRange::MergeVal(Value v1, Value v2, bool is_min) const {
  if (v1.is_known && v2.is_known) {
    if (v1.instruction == v2.instruction && v1.a_constant == v2.a_constant) {
      return Value(v1.instruction, v1.a_constant,
                   is_min ? std::min(v1.b_constant, v2.b_constant)
                          : std::max(v1.b_constant, v2.b_constant));
    }
  }
  return Value();
}

bool InductionVarRange::GenerateRangeOrLastValue(HInstruction* context,
                                                 HInstruction* instruction,
                                                 bool is_last_value,
                                                 HGraph* graph,
                                                 HBasicBlock* block,
                                                 /*out*/HInstruction** lower,
                                                 /*out*/HInstruction** upper,
                                                 /*out*/HInstruction** taken_test,
                                                 /*out*/int64_t* stride_value,
                                                 /*out*/bool* needs_finite_test,
                                                 /*out*/bool* needs_taken_test) const {
  HLoopInformation* loop = nullptr;
  HInductionVarAnalysis::InductionInfo* info = nullptr;
  HInductionVarAnalysis::InductionInfo* trip = nullptr;
  if (!HasInductionInfo(context, instruction, &loop, &info, &trip) || trip == nullptr) {
    return false;  // codegen needs all information, including tripcount
  }
  // Determine what tests are needed. A finite test is needed if the evaluation code uses the
  // trip-count and the loop maybe unsafe (because in such cases, the index could "overshoot"
  // the computed range). A taken test is needed for any unknown trip-count, even if evaluation
  // code does not use the trip-count explicitly (since there could be an implicit relation
  // between e.g. an invariant subscript and a not-taken condition).
  bool in_body = context->GetBlock() != loop->GetHeader();
  *stride_value = 0;
  *needs_finite_test = NeedsTripCount(info, stride_value) && IsUnsafeTripCount(trip);
  *needs_taken_test = IsBodyTripCount(trip);
  // Handle last value request.
  if (is_last_value) {
    DCHECK(!in_body);
    switch (info->induction_class) {
      case HInductionVarAnalysis::kLinear:
        if (*stride_value > 0) {
          lower = nullptr;
        } else {
          upper = nullptr;
        }
        break;
      case HInductionVarAnalysis::kPolynomial:
        return GenerateLastValuePolynomial(info, trip, graph, block, lower);
      case HInductionVarAnalysis::kGeometric:
        return GenerateLastValueGeometric(info, trip, graph, block, lower);
      case HInductionVarAnalysis::kWrapAround:
        return GenerateLastValueWrapAround(info, trip, graph, block, lower);
      case HInductionVarAnalysis::kPeriodic:
        return GenerateLastValuePeriodic(info, trip, graph, block, lower, needs_taken_test);
      default:
        return false;
    }
  }
  // Code generation for taken test: generate the code when requested or otherwise analyze
  // if code generation is feasible when taken test is needed.
  if (taken_test != nullptr) {
    return GenerateCode(trip->op_b, nullptr, graph, block, taken_test, in_body, /* is_min */ false);
  } else if (*needs_taken_test) {
    if (!GenerateCode(
        trip->op_b, nullptr, nullptr, nullptr, nullptr, in_body, /* is_min */ false)) {
      return false;
    }
  }
  // Code generation for lower and upper.
  return
      // Success on lower if invariant (not set), or code can be generated.
      ((info->induction_class == HInductionVarAnalysis::kInvariant) ||
          GenerateCode(info, trip, graph, block, lower, in_body, /* is_min */ true)) &&
      // And success on upper.
      GenerateCode(info, trip, graph, block, upper, in_body, /* is_min */ false);
}

bool InductionVarRange::GenerateLastValuePolynomial(HInductionVarAnalysis::InductionInfo* info,
                                                    HInductionVarAnalysis::InductionInfo* trip,
                                                    HGraph* graph,
                                                    HBasicBlock* block,
                                                    /*out*/HInstruction** result) const {
  DCHECK(info != nullptr);
  DCHECK_EQ(info->induction_class, HInductionVarAnalysis::kPolynomial);
  // Detect known coefficients and trip count (always taken).
  int64_t a = 0;
  int64_t b = 0;
  int64_t m = 0;
  if (IsConstant(info->op_a->op_a, kExact, &a) && a >= 0 &&
      IsConstant(info->op_a->op_b, kExact, &b) && b >= 0 &&
      IsConstant(trip->op_a, kExact, &m) && m >= 1) {
    // Evaluate bounds on sum_i=0^m-1(a * i + b) + c with a,b >= 0 for known
    // maximum index value m as a * (m * (m-1)) / 2 + b * m + c.
    // TODO: generalize
    HInstruction* c_instr = nullptr;
    if (GenerateCode(info->op_b, nullptr, graph, block, graph ? &c_instr : nullptr, false, false)) {
      if (graph != nullptr) {
        int64_t sum = a * ((m * (m - 1)) / 2) + b * m;
        *result = Insert(block, new (graph->GetArena()) HAdd(info->type,
                                                             graph->GetIntConstant(sum), c_instr));
      }
      return true;
    }
  }
  return false;
}

bool InductionVarRange::GenerateLastValueGeometric(HInductionVarAnalysis::InductionInfo* info,
                                                   HInductionVarAnalysis::InductionInfo* trip,
                                                   HGraph* graph,
                                                   HBasicBlock* block,
                                                   /*out*/HInstruction** result) const {
  DCHECK(info != nullptr);
  DCHECK_EQ(info->induction_class, HInductionVarAnalysis::kGeometric);
  // Detect known base and trip count (always taken).
  int64_t f = 0;
  int64_t t = 0;
  if (IsIntAndGet(info->fetch, &f) && f >= 1 && IsConstant(trip->op_a, kExact, &t) && t >= 1) {
    HInstruction* opa = nullptr;
    HInstruction* opb = nullptr;
    if (GenerateCode(info->op_a, nullptr, graph, block, &opa, false, false) &&
        GenerateCode(info->op_b, nullptr, graph, block, &opb, false, false)) {
      // Compute f ^ t.
      int64_t fpowt = IntPow(f, t);
      if (graph != nullptr) {
        DCHECK(info->type == Primitive::kPrimInt);  // due to codegen, generalize?
        if (fpowt == 0) {
          // Special case: repeated mul/div always yields zero.
          *result = graph->GetIntConstant(0);
        } else if (info->operation == HInductionVarAnalysis::kMul) {
          // Last value multiplication: a * f ^ t + b.
          HInstruction* mul = Insert(block,
                                     new (graph->GetArena()) HMul(info->type,
                                                                  opa,
                                                                  graph->GetIntConstant(fpowt)));
          *result = Insert(block, new (graph->GetArena()) HAdd(info->type, mul, opb));
        } else {
          // Last value multiplication: a * f ^ -t + b.
          DCHECK_EQ(info->operation, HInductionVarAnalysis::kDiv);
          HInstruction* div = Insert(block,
                                     new (graph->GetArena()) HDiv(info->type,
                                                                  opa,
                                                                  graph->GetIntConstant(fpowt),
                                                                  kNoDexPc));
          *result = Insert(block, new (graph->GetArena()) HAdd(info->type, div, opb));
        }
      }
      return true;
    }
  }
  return false;
}

bool InductionVarRange::GenerateLastValueWrapAround(HInductionVarAnalysis::InductionInfo* info,
                                                    HInductionVarAnalysis::InductionInfo* trip,
                                                    HGraph* graph,
                                                    HBasicBlock* block,
                                                    /*out*/HInstruction** result) const {
  DCHECK(info != nullptr);
  DCHECK_EQ(info->induction_class, HInductionVarAnalysis::kWrapAround);
  // Count depth.
  int32_t depth = 0;
  for (; info->induction_class == HInductionVarAnalysis::kWrapAround;
       info = info->op_b, ++depth) {}
  // Handle wrap(x, wrap(.., y)) if trip count reaches an invariant at end.
  // TODO: generalize
  int64_t t = 0;
  if (info->induction_class == HInductionVarAnalysis::kInvariant &&
      IsConstant(trip->op_a, kExact, &t) && t >= depth &&
      GenerateCode(info, nullptr, graph, block, result, false, false)) {
    return true;
  }
  return false;
}

bool InductionVarRange::GenerateLastValuePeriodic(HInductionVarAnalysis::InductionInfo* info,
                                                  HInductionVarAnalysis::InductionInfo* trip,
                                                  HGraph* graph,
                                                  HBasicBlock* block,
                                                  /*out*/HInstruction** result,
                                                  /*out*/bool* needs_taken_test) const {
  DCHECK(info != nullptr);
  DCHECK_EQ(info->induction_class, HInductionVarAnalysis::kPeriodic);
  // Count period.
  int32_t period = 1;
  for (HInductionVarAnalysis::InductionInfo* p = info;
       p->induction_class == HInductionVarAnalysis::kPeriodic;
       p = p->op_b, ++period) {}
  // Handle periodic(x, y) case for restricted types.
  // TODO: generalize
  if (period != 2 ||
      trip->op_a->type != Primitive::kPrimInt ||
      (info->type != Primitive::kPrimInt && info->type != Primitive::kPrimBoolean)) {
    return false;
  }
  HInstruction* x_instr = nullptr;
  HInstruction* y_instr = nullptr;
  HInstruction* trip_expr = nullptr;
  if (GenerateCode(info->op_a, nullptr, graph, block, graph ? &x_instr   : nullptr, false, false) &&
      GenerateCode(info->op_b, nullptr, graph, block, graph ? &y_instr   : nullptr, false, false) &&
      GenerateCode(trip->op_a, nullptr, graph, block, graph ? &trip_expr : nullptr, false, false)) {
    // During actual code generation (graph != nullptr),
    // generate is_even ? x : y select instruction.
    if (graph != nullptr) {
      HInstruction* is_even = Insert(block, new (graph->GetArena()) HEqual(
          Insert(block, new (graph->GetArena()) HAnd(
              Primitive::kPrimInt, trip_expr, graph->GetIntConstant(1))),
          graph->GetIntConstant(0), kNoDexPc));
      *result = Insert(block, new (graph->GetArena()) HSelect(is_even, x_instr, y_instr, kNoDexPc));
    }
    // Guard select with taken test if needed.
    if (*needs_taken_test) {
      HInstruction* taken_test = nullptr;
      if (!GenerateCode(
          trip->op_b, nullptr, graph, block, graph ? &taken_test : nullptr, false, false)) {
        return false;
      } else if (graph != nullptr) {
         *result = Insert(block,
                          new (graph->GetArena()) HSelect(taken_test, *result, x_instr, kNoDexPc));
      }
      *needs_taken_test = false;  // taken care of
    }
    return true;
  }
  return false;
}

bool InductionVarRange::GenerateCode(HInductionVarAnalysis::InductionInfo* info,
                                     HInductionVarAnalysis::InductionInfo* trip,
                                     HGraph* graph,  // when set, code is generated
                                     HBasicBlock* block,
                                     /*out*/HInstruction** result,
                                     bool in_body,
                                     bool is_min) const {
  if (info != nullptr) {
    // If during codegen, the result is not needed (nullptr), simply return success.
    if (graph != nullptr && result == nullptr) {
      return true;
    }
    // Verify type safety.
    // TODO: generalize
    Primitive::Type type = Primitive::kPrimInt;
    if (info->type != Primitive::kPrimInt && info->type != Primitive::kPrimBoolean) {
      return false;
    }
    // Handle current operation.
    HInstruction* opa = nullptr;
    HInstruction* opb = nullptr;
    switch (info->induction_class) {
      case HInductionVarAnalysis::kInvariant:
        // Invariants (note that even though is_min does not impact code generation for
        // invariants, some effort is made to keep this parameter consistent).
        switch (info->operation) {
          case HInductionVarAnalysis::kAdd:
          case HInductionVarAnalysis::kRem:  // no proper is_min for second arg
          case HInductionVarAnalysis::kXor:  // no proper is_min for second arg
          case HInductionVarAnalysis::kLT:
          case HInductionVarAnalysis::kLE:
          case HInductionVarAnalysis::kGT:
          case HInductionVarAnalysis::kGE:
            if (GenerateCode(info->op_a, trip, graph, block, &opa, in_body, is_min) &&
                GenerateCode(info->op_b, trip, graph, block, &opb, in_body, is_min)) {
              if (graph != nullptr) {
                HInstruction* operation = nullptr;
                switch (info->operation) {
                  case HInductionVarAnalysis::kAdd:
                    operation = new (graph->GetArena()) HAdd(type, opa, opb); break;
                  case HInductionVarAnalysis::kRem:
                    operation = new (graph->GetArena()) HRem(type, opa, opb, kNoDexPc); break;
                  case HInductionVarAnalysis::kXor:
                    operation = new (graph->GetArena()) HXor(type, opa, opb); break;
                  case HInductionVarAnalysis::kLT:
                    operation = new (graph->GetArena()) HLessThan(opa, opb); break;
                  case HInductionVarAnalysis::kLE:
                    operation = new (graph->GetArena()) HLessThanOrEqual(opa, opb); break;
                  case HInductionVarAnalysis::kGT:
                    operation = new (graph->GetArena()) HGreaterThan(opa, opb); break;
                  case HInductionVarAnalysis::kGE:
                    operation = new (graph->GetArena()) HGreaterThanOrEqual(opa, opb); break;
                  default:
                    LOG(FATAL) << "unknown operation";
                }
                *result = Insert(block, operation);
              }
              return true;
            }
            break;
          case HInductionVarAnalysis::kSub:  // second reversed!
            if (GenerateCode(info->op_a, trip, graph, block, &opa, in_body, is_min) &&
                GenerateCode(info->op_b, trip, graph, block, &opb, in_body, !is_min)) {
              if (graph != nullptr) {
                *result = Insert(block, new (graph->GetArena()) HSub(type, opa, opb));
              }
              return true;
            }
            break;
          case HInductionVarAnalysis::kNeg:  // reversed!
            if (GenerateCode(info->op_b, trip, graph, block, &opb, in_body, !is_min)) {
              if (graph != nullptr) {
                *result = Insert(block, new (graph->GetArena()) HNeg(type, opb));
              }
              return true;
            }
            break;
          case HInductionVarAnalysis::kFetch:
            if (graph != nullptr) {
              *result = info->fetch;  // already in HIR
            }
            return true;
          case HInductionVarAnalysis::kTripCountInLoop:
          case HInductionVarAnalysis::kTripCountInLoopUnsafe:
            if (!in_body && !is_min) {  // one extra!
              return GenerateCode(info->op_a, trip, graph, block, result, in_body, is_min);
            }
            FALLTHROUGH_INTENDED;
          case HInductionVarAnalysis::kTripCountInBody:
          case HInductionVarAnalysis::kTripCountInBodyUnsafe:
            if (is_min) {
              if (graph != nullptr) {
                *result = graph->GetIntConstant(0);
              }
              return true;
            } else if (in_body) {
              if (GenerateCode(info->op_a, trip, graph, block, &opb, in_body, is_min)) {
                if (graph != nullptr) {
                  *result = Insert(block,
                                   new (graph->GetArena())
                                       HSub(type, opb, graph->GetIntConstant(1)));
                }
                return true;
              }
            }
            break;
          default:
            break;
        }
        break;
      case HInductionVarAnalysis::kLinear: {
        // Linear induction a * i + b, for normalized 0 <= i < TC. For ranges, this should
        // be restricted to a unit stride to avoid arithmetic wrap-around situations that
        // are harder to guard against. For a last value, requesting min/max based on any
        // stride yields right value.
        int64_t stride_value = 0;
        if (IsConstant(info->op_a, kExact, &stride_value)) {
          const bool is_min_a = stride_value >= 0 ? is_min : !is_min;
          if (GenerateCode(trip,       trip, graph, block, &opa, in_body, is_min_a) &&
              GenerateCode(info->op_b, trip, graph, block, &opb, in_body, is_min)) {
            if (graph != nullptr) {
              HInstruction* oper;
              if (stride_value == 1) {
                oper = new (graph->GetArena()) HAdd(type, opa, opb);
              } else if (stride_value == -1) {
                oper = new (graph->GetArena()) HSub(type, opb, opa);
              } else {
                HInstruction* mul = new (graph->GetArena()) HMul(
                    type, graph->GetIntConstant(stride_value), opa);
                oper = new (graph->GetArena()) HAdd(type, Insert(block, mul), opb);
              }
              *result = Insert(block, oper);
            }
            return true;
          }
        }
        break;
      }
      case HInductionVarAnalysis::kPolynomial:
      case HInductionVarAnalysis::kGeometric:
        break;
      case HInductionVarAnalysis::kWrapAround:
      case HInductionVarAnalysis::kPeriodic: {
        // Wrap-around and periodic inductions are restricted to constants only, so that extreme
        // values are easy to test at runtime without complications of arithmetic wrap-around.
        Value extreme = GetVal(info, trip, in_body, is_min);
        if (IsConstantValue(extreme)) {
          if (graph != nullptr) {
            *result = graph->GetIntConstant(extreme.b_constant);
          }
          return true;
        }
        break;
      }
    }
  }
  return false;
}

void InductionVarRange::ReplaceInduction(HInductionVarAnalysis::InductionInfo* info,
                                         HInstruction* fetch,
                                         HInstruction* replacement) {
  if (info != nullptr) {
    if (info->induction_class == HInductionVarAnalysis::kInvariant &&
        info->operation == HInductionVarAnalysis::kFetch &&
        info->fetch == fetch) {
      info->fetch = replacement;
    }
    ReplaceInduction(info->op_a, fetch, replacement);
    ReplaceInduction(info->op_b, fetch, replacement);
  }
}

}  // namespace art
