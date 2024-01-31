// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "arrow/array/builder_primitive.h"
#include "arrow/array/concatenate.h"
#include "arrow/buffer_builder.h"
#include "arrow/chunk_resolver.h"
#include "arrow/chunked_array.h"
#include "arrow/compute/api_vector.h"
#include "arrow/compute/kernels/codegen_internal.h"
#include "arrow/compute/kernels/vector_selection_internal.h"
#include "arrow/compute/kernels/vector_selection_take_internal.h"
#include "arrow/memory_pool.h"
#include "arrow/record_batch.h"
#include "arrow/table.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/bit_block_counter.h"
#include "arrow/util/bit_run_reader.h"
#include "arrow/util/bit_util.h"
#include "arrow/util/int_util.h"
#include "arrow/util/ree_util.h"

namespace arrow {

using internal::BinaryBitBlockCounter;
using internal::BitBlockCount;
using internal::BitBlockCounter;
using internal::CheckIndexBounds;
using internal::ChunkLocation;
using internal::ChunkResolver;
using internal::OptionalBitBlockCounter;

namespace compute {
namespace internal {

namespace {

template <typename IndexType>
Result<std::shared_ptr<ArrayData>> GetTakeIndicesFromBitmapImpl(
    const ArraySpan& filter, FilterOptions::NullSelectionBehavior null_selection,
    MemoryPool* memory_pool) {
  using T = typename IndexType::c_type;

  const uint8_t* filter_data = filter.buffers[1].data;
  const bool have_filter_nulls = filter.MayHaveNulls();
  const uint8_t* filter_is_valid = filter.buffers[0].data;

  if (have_filter_nulls && null_selection == FilterOptions::EMIT_NULL) {
    // Most complex case: the filter may have nulls and we don't drop them.
    // The logic is ternary:
    // - filter is null: emit null
    // - filter is valid and true: emit index
    // - filter is valid and false: don't emit anything

    typename TypeTraits<IndexType>::BuilderType builder(memory_pool);

    // The position relative to the start of the filter
    T position = 0;
    // The current position taking the filter offset into account
    int64_t position_with_offset = filter.offset;

    // To count blocks where filter_data[i] || !filter_is_valid[i]
    BinaryBitBlockCounter filter_counter(filter_data, filter.offset, filter_is_valid,
                                         filter.offset, filter.length);
    BitBlockCounter is_valid_counter(filter_is_valid, filter.offset, filter.length);
    while (position < filter.length) {
      // true OR NOT valid
      BitBlockCount selected_or_null_block = filter_counter.NextOrNotWord();
      if (selected_or_null_block.NoneSet()) {
        position += selected_or_null_block.length;
        position_with_offset += selected_or_null_block.length;
        continue;
      }
      RETURN_NOT_OK(builder.Reserve(selected_or_null_block.popcount));

      // If the values are all valid and the selected_or_null_block is full,
      // then we can infer that all the values are true and skip the bit checking
      BitBlockCount is_valid_block = is_valid_counter.NextWord();

      if (selected_or_null_block.AllSet() && is_valid_block.AllSet()) {
        // All the values are selected and non-null
        for (int64_t i = 0; i < selected_or_null_block.length; ++i) {
          builder.UnsafeAppend(position++);
        }
        position_with_offset += selected_or_null_block.length;
      } else {
        // Some of the values are false or null
        for (int64_t i = 0; i < selected_or_null_block.length; ++i) {
          if (bit_util::GetBit(filter_is_valid, position_with_offset)) {
            if (bit_util::GetBit(filter_data, position_with_offset)) {
              builder.UnsafeAppend(position);
            }
          } else {
            // Null slot, so append a null
            builder.UnsafeAppendNull();
          }
          ++position;
          ++position_with_offset;
        }
      }
    }
    std::shared_ptr<ArrayData> result;
    RETURN_NOT_OK(builder.FinishInternal(&result));
    return result;
  }

  // Other cases don't emit nulls and are therefore simpler.
  TypedBufferBuilder<T> builder(memory_pool);

  if (have_filter_nulls) {
    // The filter may have nulls, so we scan the validity bitmap and the filter
    // data bitmap together.
    DCHECK_EQ(null_selection, FilterOptions::DROP);

    // The position relative to the start of the filter
    T position = 0;
    // The current position taking the filter offset into account
    int64_t position_with_offset = filter.offset;

    BinaryBitBlockCounter filter_counter(filter_data, filter.offset, filter_is_valid,
                                         filter.offset, filter.length);
    while (position < filter.length) {
      BitBlockCount and_block = filter_counter.NextAndWord();
      RETURN_NOT_OK(builder.Reserve(and_block.popcount));
      if (and_block.AllSet()) {
        // All the values are selected and non-null
        for (int64_t i = 0; i < and_block.length; ++i) {
          builder.UnsafeAppend(position++);
        }
        position_with_offset += and_block.length;
      } else if (!and_block.NoneSet()) {
        // Some of the values are false or null
        for (int64_t i = 0; i < and_block.length; ++i) {
          if (bit_util::GetBit(filter_is_valid, position_with_offset) &&
              bit_util::GetBit(filter_data, position_with_offset)) {
            builder.UnsafeAppend(position);
          }
          ++position;
          ++position_with_offset;
        }
      } else {
        position += and_block.length;
        position_with_offset += and_block.length;
      }
    }
  } else {
    // The filter has no nulls, so we need only look for true values
    RETURN_NOT_OK(::arrow::internal::VisitSetBitRuns(
        filter_data, filter.offset, filter.length, [&](int64_t offset, int64_t length) {
          // Append the consecutive run of indices
          RETURN_NOT_OK(builder.Reserve(length));
          for (int64_t i = 0; i < length; ++i) {
            builder.UnsafeAppend(static_cast<T>(offset + i));
          }
          return Status::OK();
        }));
  }

  const int64_t length = builder.length();
  std::shared_ptr<Buffer> out_buffer;
  RETURN_NOT_OK(builder.Finish(&out_buffer));
  return std::make_shared<ArrayData>(TypeTraits<IndexType>::type_singleton(), length,
                                     BufferVector{nullptr, out_buffer}, /*null_count=*/0);
}

template <typename RunEndType>
Result<std::shared_ptr<ArrayData>> GetTakeIndicesFromREEBitmapImpl(
    const ArraySpan& filter, FilterOptions::NullSelectionBehavior null_selection,
    MemoryPool* memory_pool) {
  using T = typename RunEndType::c_type;
  const ArraySpan& filter_values = ::arrow::ree_util::ValuesArray(filter);
  const int64_t filter_values_offset = filter_values.offset;
  const uint8_t* filter_is_valid = filter_values.buffers[0].data;
  const uint8_t* filter_selection = filter_values.buffers[1].data;
  const bool filter_may_have_nulls = filter_values.MayHaveNulls();

  // BinaryBitBlockCounter is not used here because a REE bitmap, if built
  // correctly, is not going to have long continuous runs of 0s or 1s in the
  // values array.

  const ::arrow::ree_util::RunEndEncodedArraySpan<T> filter_span(filter);
  auto it = filter_span.begin();
  if (filter_may_have_nulls && null_selection == FilterOptions::EMIT_NULL) {
    // Most complex case: the filter may have nulls and we don't drop them.
    // The logic is ternary:
    // - filter is null: emit null
    // - filter is valid and true: emit index
    // - filter is valid and false: don't emit anything

    typename TypeTraits<RunEndType>::BuilderType builder(memory_pool);
    for (; !it.is_end(filter_span); ++it) {
      const int64_t position_with_offset = filter_values_offset + it.index_into_array();
      const bool is_null = !bit_util::GetBit(filter_is_valid, position_with_offset);
      if (is_null) {
        RETURN_NOT_OK(builder.AppendNulls(it.run_length()));
      } else {
        const bool emit_run = bit_util::GetBit(filter_selection, position_with_offset);
        if (emit_run) {
          const int64_t run_end = it.run_end();
          RETURN_NOT_OK(builder.Reserve(run_end - it.logical_position()));
          for (int64_t position = it.logical_position(); position < run_end; position++) {
            builder.UnsafeAppend(static_cast<T>(position));
          }
        }
      }
    }
    std::shared_ptr<ArrayData> result;
    RETURN_NOT_OK(builder.FinishInternal(&result));
    return result;
  }

  // Other cases don't emit nulls and are therefore simpler.
  TypedBufferBuilder<T> builder(memory_pool);

  if (filter_may_have_nulls) {
    DCHECK_EQ(null_selection, FilterOptions::DROP);
    // The filter may have nulls, so we scan the validity bitmap and the filter
    // data bitmap together.
    for (; !it.is_end(filter_span); ++it) {
      const int64_t position_with_offset = filter_values_offset + it.index_into_array();
      const bool emit_run = bit_util::GetBit(filter_is_valid, position_with_offset) &&
                            bit_util::GetBit(filter_selection, position_with_offset);
      if (emit_run) {
        const int64_t run_end = it.run_end();
        RETURN_NOT_OK(builder.Reserve(run_end - it.logical_position()));
        for (int64_t position = it.logical_position(); position < run_end; position++) {
          builder.UnsafeAppend(static_cast<T>(position));
        }
      }
    }
  } else {
    // The filter has no nulls, so we need only look for true values
    for (; !it.is_end(filter_span); ++it) {
      const int64_t position_with_offset = filter_values_offset + it.index_into_array();
      const bool emit_run = bit_util::GetBit(filter_selection, position_with_offset);
      if (emit_run) {
        const int64_t run_end = it.run_end();
        RETURN_NOT_OK(builder.Reserve(run_end - it.logical_position()));
        for (int64_t position = it.logical_position(); position < run_end; position++) {
          builder.UnsafeAppend(static_cast<T>(position));
        }
      }
    }
  }

  const int64_t length = builder.length();
  std::shared_ptr<Buffer> out_buffer;
  RETURN_NOT_OK(builder.Finish(&out_buffer));
  return std::make_shared<ArrayData>(TypeTraits<RunEndType>::type_singleton(), length,
                                     BufferVector{nullptr, std::move(out_buffer)},
                                     /*null_count=*/0);
}

Result<std::shared_ptr<ArrayData>> GetTakeIndicesFromBitmap(
    const ArraySpan& filter, FilterOptions::NullSelectionBehavior null_selection,
    MemoryPool* memory_pool) {
  DCHECK_EQ(filter.type->id(), Type::BOOL);
  if (filter.length <= std::numeric_limits<uint16_t>::max()) {
    return GetTakeIndicesFromBitmapImpl<UInt16Type>(filter, null_selection, memory_pool);
  } else if (filter.length <= std::numeric_limits<uint32_t>::max()) {
    return GetTakeIndicesFromBitmapImpl<UInt32Type>(filter, null_selection, memory_pool);
  } else {
    // Arrays over 4 billion elements, not especially likely.
    return Status::NotImplemented(
        "Filter length exceeds UINT32_MAX, "
        "consider a different strategy for selecting elements");
  }
}

Result<std::shared_ptr<ArrayData>> GetTakeIndicesFromREEBitmap(
    const ArraySpan& filter, FilterOptions::NullSelectionBehavior null_selection,
    MemoryPool* memory_pool) {
  const auto& ree_type = checked_cast<const RunEndEncodedType&>(*filter.type);
  // The resulting array will contain indexes of the same type as the run-end type of the
  // run-end encoded filter. Run-end encoded arrays have to pick the smallest run-end type
  // to maximize memory savings, so we can be re-use that decision here and get a good
  // result without checking the logical length of the filter.
  switch (ree_type.run_end_type()->id()) {
    case Type::INT16:
      return GetTakeIndicesFromREEBitmapImpl<Int16Type>(filter, null_selection,
                                                        memory_pool);
    case Type::INT32:
      return GetTakeIndicesFromREEBitmapImpl<Int32Type>(filter, null_selection,
                                                        memory_pool);
    default:
      DCHECK_EQ(ree_type.run_end_type()->id(), Type::INT64);
      return GetTakeIndicesFromREEBitmapImpl<Int64Type>(filter, null_selection,
                                                        memory_pool);
  }
}

}  // namespace

Result<std::shared_ptr<ArrayData>> GetTakeIndices(
    const ArraySpan& filter, FilterOptions::NullSelectionBehavior null_selection,
    MemoryPool* memory_pool) {
  if (filter.type->id() == Type::BOOL) {
    return GetTakeIndicesFromBitmap(filter, null_selection, memory_pool);
  }
  return GetTakeIndicesFromREEBitmap(filter, null_selection, memory_pool);
}

namespace {

using TakeState = OptionsWrapper<TakeOptions>;

// ----------------------------------------------------------------------
// Implement optimized take for primitive types from boolean to 1/2/4/8-byte
// C-type based types. Use common implementation for every byte width and only
// generate code for unsigned integer indices, since after boundschecking to
// check for negative numbers in the indices we can safely reinterpret_cast
// signed integers as unsigned.

/// \brief The Take implementation for primitive (fixed-width) types does not
/// use the logical Arrow type but rather the physical C type. This way we
/// only generate one take function for each byte width.
///
/// This function assumes that the indices have been boundschecked.
template <typename IndexCType, typename ValueWidthConstant>
struct PrimitiveTakeImpl {
  static constexpr int kValueWidth = ValueWidthConstant::value;

  static void Exec(const ArraySpan& values, const ArraySpan& indices,
                   ArrayData* out_arr) {
    DCHECK_EQ(values.type->byte_width(), kValueWidth);
    const auto* values_data =
        values.GetValues<uint8_t>(1, 0) + kValueWidth * values.offset;
    const uint8_t* values_is_valid = values.buffers[0].data;
    auto values_offset = values.offset;

    const auto* indices_data = indices.GetValues<IndexCType>(1);
    const uint8_t* indices_is_valid = indices.buffers[0].data;
    auto indices_offset = indices.offset;

    auto out = out_arr->GetMutableValues<uint8_t>(1, 0) + kValueWidth * out_arr->offset;
    auto out_is_valid = out_arr->buffers[0]->mutable_data();
    auto out_offset = out_arr->offset;
    DCHECK_EQ(out_offset, 0);

    // If either the values or indices have nulls, we preemptively zero out the
    // out validity bitmap so that we don't have to use ClearBit in each
    // iteration for nulls.
    if (values.null_count != 0 || indices.null_count != 0) {
      bit_util::SetBitsTo(out_is_valid, out_offset, indices.length, false);
    }

    auto WriteValue = [&](int64_t position) {
      memcpy(out + position * kValueWidth,
             values_data + indices_data[position] * kValueWidth, kValueWidth);
    };

    auto WriteZero = [&](int64_t position) {
      memset(out + position * kValueWidth, 0, kValueWidth);
    };

    auto WriteZeroSegment = [&](int64_t position, int64_t length) {
      memset(out + position * kValueWidth, 0, kValueWidth * length);
    };

    OptionalBitBlockCounter indices_bit_counter(indices_is_valid, indices_offset,
                                                indices.length);
    int64_t position = 0;
    int64_t valid_count = 0;
    while (position < indices.length) {
      BitBlockCount block = indices_bit_counter.NextBlock();
      if (values.null_count == 0) {
        // Values are never null, so things are easier
        valid_count += block.popcount;
        if (block.popcount == block.length) {
          // Fastest path: neither values nor index nulls
          bit_util::SetBitsTo(out_is_valid, out_offset + position, block.length, true);
          for (int64_t i = 0; i < block.length; ++i) {
            WriteValue(position);
            ++position;
          }
        } else if (block.popcount > 0) {
          // Slow path: some indices but not all are null
          for (int64_t i = 0; i < block.length; ++i) {
            if (bit_util::GetBit(indices_is_valid, indices_offset + position)) {
              // index is not null
              bit_util::SetBit(out_is_valid, out_offset + position);
              WriteValue(position);
            } else {
              WriteZero(position);
            }
            ++position;
          }
        } else {
          WriteZeroSegment(position, block.length);
          position += block.length;
        }
      } else {
        // Values have nulls, so we must do random access into the values bitmap
        if (block.popcount == block.length) {
          // Faster path: indices are not null but values may be
          for (int64_t i = 0; i < block.length; ++i) {
            if (bit_util::GetBit(values_is_valid,
                                 values_offset + indices_data[position])) {
              // value is not null
              WriteValue(position);
              bit_util::SetBit(out_is_valid, out_offset + position);
              ++valid_count;
            } else {
              WriteZero(position);
            }
            ++position;
          }
        } else if (block.popcount > 0) {
          // Slow path: some but not all indices are null. Since we are doing
          // random access in general we have to check the value nullness one by
          // one.
          for (int64_t i = 0; i < block.length; ++i) {
            if (bit_util::GetBit(indices_is_valid, indices_offset + position) &&
                bit_util::GetBit(values_is_valid,
                                 values_offset + indices_data[position])) {
              // index is not null && value is not null
              WriteValue(position);
              bit_util::SetBit(out_is_valid, out_offset + position);
              ++valid_count;
            } else {
              WriteZero(position);
            }
            ++position;
          }
        } else {
          WriteZeroSegment(position, block.length);
          position += block.length;
        }
      }
    }
    out_arr->null_count = out_arr->length - valid_count;
  }
};

template <typename IndexCType>
struct BooleanTakeImpl {
  static void Exec(const ArraySpan& values, const ArraySpan& indices,
                   ArrayData* out_arr) {
    const uint8_t* values_data = values.buffers[1].data;
    const uint8_t* values_is_valid = values.buffers[0].data;
    auto values_offset = values.offset;

    const auto* indices_data = indices.GetValues<IndexCType>(1);
    const uint8_t* indices_is_valid = indices.buffers[0].data;
    auto indices_offset = indices.offset;

    auto out = out_arr->buffers[1]->mutable_data();
    auto out_is_valid = out_arr->buffers[0]->mutable_data();
    auto out_offset = out_arr->offset;

    // If either the values or indices have nulls, we preemptively zero out the
    // out validity bitmap so that we don't have to use ClearBit in each
    // iteration for nulls.
    if (values.null_count != 0 || indices.null_count != 0) {
      bit_util::SetBitsTo(out_is_valid, out_offset, indices.length, false);
    }
    // Avoid uninitialized data in values array
    bit_util::SetBitsTo(out, out_offset, indices.length, false);

    auto PlaceDataBit = [&](int64_t loc, IndexCType index) {
      bit_util::SetBitTo(out, out_offset + loc,
                         bit_util::GetBit(values_data, values_offset + index));
    };

    OptionalBitBlockCounter indices_bit_counter(indices_is_valid, indices_offset,
                                                indices.length);
    int64_t position = 0;
    int64_t valid_count = 0;
    while (position < indices.length) {
      BitBlockCount block = indices_bit_counter.NextBlock();
      if (values.null_count == 0) {
        // Values are never null, so things are easier
        valid_count += block.popcount;
        if (block.popcount == block.length) {
          // Fastest path: neither values nor index nulls
          bit_util::SetBitsTo(out_is_valid, out_offset + position, block.length, true);
          for (int64_t i = 0; i < block.length; ++i) {
            PlaceDataBit(position, indices_data[position]);
            ++position;
          }
        } else if (block.popcount > 0) {
          // Slow path: some but not all indices are null
          for (int64_t i = 0; i < block.length; ++i) {
            if (bit_util::GetBit(indices_is_valid, indices_offset + position)) {
              // index is not null
              bit_util::SetBit(out_is_valid, out_offset + position);
              PlaceDataBit(position, indices_data[position]);
            }
            ++position;
          }
        } else {
          position += block.length;
        }
      } else {
        // Values have nulls, so we must do random access into the values bitmap
        if (block.popcount == block.length) {
          // Faster path: indices are not null but values may be
          for (int64_t i = 0; i < block.length; ++i) {
            if (bit_util::GetBit(values_is_valid,
                                 values_offset + indices_data[position])) {
              // value is not null
              bit_util::SetBit(out_is_valid, out_offset + position);
              PlaceDataBit(position, indices_data[position]);
              ++valid_count;
            }
            ++position;
          }
        } else if (block.popcount > 0) {
          // Slow path: some but not all indices are null. Since we are doing
          // random access in general we have to check the value nullness one by
          // one.
          for (int64_t i = 0; i < block.length; ++i) {
            if (bit_util::GetBit(indices_is_valid, indices_offset + position)) {
              // index is not null
              if (bit_util::GetBit(values_is_valid,
                                   values_offset + indices_data[position])) {
                // value is not null
                PlaceDataBit(position, indices_data[position]);
                bit_util::SetBit(out_is_valid, out_offset + position);
                ++valid_count;
              }
            }
            ++position;
          }
        } else {
          position += block.length;
        }
      }
    }
    out_arr->null_count = out_arr->length - valid_count;
  }
};

template <template <typename...> class TakeImpl, typename... Args>
void TakeIndexDispatch(const ArraySpan& values, const ArraySpan& indices,
                       ArrayData* out) {
  // With the simplifying assumption that boundschecking has taken place
  // already at a higher level, we can now assume that the index values are all
  // non-negative. Thus, we can interpret signed integers as unsigned and avoid
  // having to generate double the amount of binary code to handle each integer
  // width.
  switch (indices.type->byte_width()) {
    case 1:
      return TakeImpl<uint8_t, Args...>::Exec(values, indices, out);
    case 2:
      return TakeImpl<uint16_t, Args...>::Exec(values, indices, out);
    case 4:
      return TakeImpl<uint32_t, Args...>::Exec(values, indices, out);
    case 8:
      return TakeImpl<uint64_t, Args...>::Exec(values, indices, out);
    default:
      DCHECK(false) << "Invalid indices byte width";
      break;
  }
}

}  // namespace

Status PrimitiveTakeExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  const ArraySpan& values = batch[0].array;
  const ArraySpan& indices = batch[1].array;

  if (TakeState::Get(ctx).boundscheck) {
    RETURN_NOT_OK(CheckIndexBounds(indices, values.length));
  }

  ArrayData* out_arr = out->array_data().get();

  const int bit_width = values.type->bit_width();

  // TODO: When neither values nor indices contain nulls, we can skip
  // allocating the validity bitmap altogether and save time and space. A
  // streamlined PrimitiveTakeImpl would need to be written that skips all
  // interactions with the output validity bitmap, though.
  RETURN_NOT_OK(PreallocatePrimitiveArrayData(ctx, indices.length, bit_width,
                                              /*allocate_validity=*/true, out_arr));
  switch (bit_width) {
    case 1:
      TakeIndexDispatch<BooleanTakeImpl>(values, indices, out_arr);
      break;
    case 8:
      TakeIndexDispatch<PrimitiveTakeImpl, std::integral_constant<int, 1>>(
          values, indices, out_arr);
      break;
    case 16:
      TakeIndexDispatch<PrimitiveTakeImpl, std::integral_constant<int, 2>>(
          values, indices, out_arr);
      break;
    case 32:
      TakeIndexDispatch<PrimitiveTakeImpl, std::integral_constant<int, 4>>(
          values, indices, out_arr);
      break;
    case 64:
      TakeIndexDispatch<PrimitiveTakeImpl, std::integral_constant<int, 8>>(
          values, indices, out_arr);
      break;
    case 128:
      // For INTERVAL_MONTH_DAY_NANO, DECIMAL128
      TakeIndexDispatch<PrimitiveTakeImpl, std::integral_constant<int, 16>>(
          values, indices, out_arr);
      break;
    case 256:
      // For DECIMAL256
      TakeIndexDispatch<PrimitiveTakeImpl, std::integral_constant<int, 32>>(
          values, indices, out_arr);
      break;
    default:
      return Status::NotImplemented("Unsupported primitive type for take: ",
                                    *values.type);
  }
  return Status::OK();
}

namespace {

// ----------------------------------------------------------------------
// Null take

Status NullTakeExec(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  if (TakeState::Get(ctx).boundscheck) {
    RETURN_NOT_OK(CheckIndexBounds(batch[1].array, batch[0].length()));
  }
  // batch.length doesn't take into account the take indices
  auto new_length = batch[1].array.length;
  out->value = std::make_shared<NullArray>(new_length)->data();
  return Status::OK();
}

// ----------------------------------------------------------------------
// Dictionary take

Status DictionaryTake(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  DictionaryArray values(batch[0].array.ToArrayData());
  Datum result;
  RETURN_NOT_OK(Take(Datum(values.indices()), batch[1].array.ToArrayData(),
                     TakeState::Get(ctx), ctx->exec_context())
                    .Value(&result));
  DictionaryArray taken_values(values.type(), result.make_array(), values.dictionary());
  out->value = taken_values.data();
  return Status::OK();
}

// ----------------------------------------------------------------------
// Extension take

Status ExtensionTake(KernelContext* ctx, const ExecSpan& batch, ExecResult* out) {
  ExtensionArray values(batch[0].array.ToArrayData());
  Datum result;
  RETURN_NOT_OK(Take(Datum(values.storage()), batch[1].array.ToArrayData(),
                     TakeState::Get(ctx), ctx->exec_context())
                    .Value(&result));
  ExtensionArray taken_values(values.type(), result.make_array());
  out->value = taken_values.data();
  return Status::OK();
}

// ----------------------------------------------------------------------
// Take metafunction implementation

// Shorthand naming of these functions
// A -> Array
// C -> ChunkedArray
// R -> RecordBatch
// T -> Table

Result<std::shared_ptr<ArrayData>> TakeAA(const std::shared_ptr<ArrayData>& values,
                                          const std::shared_ptr<ArrayData>& indices,
                                          const TakeOptions& options, ExecContext* ctx) {
  ARROW_ASSIGN_OR_RAISE(Datum result,
                        CallFunction("array_take", {values, indices}, &options, ctx));
  return result.array();
}

Result<std::shared_ptr<ChunkedArray>> TakeCA(const ChunkedArray& values,
                                             const Array& indices,
                                             const TakeOptions& options,
                                             ExecContext* ctx) {
  auto num_chunks = values.num_chunks();
  auto num_indices = indices.length();

  if (indices.length() == 0) {
    // Case 0: No indices were provided, nothing to take so return an empty chunked array
    return ChunkedArray::MakeEmpty(values.type());
  } else if (num_chunks < 2) {
    std::shared_ptr<Array> current_chunk;
    // Case 1: `values` is empty or has a single chunk, so just use it
    if (values.chunks().empty()) {
      ARROW_ASSIGN_OR_RAISE(current_chunk, MakeArrayOfNull(values.type(), /*length=*/0,
                                                           ctx->memory_pool()));
    } else {
      current_chunk = values.chunk(0);
    }
    // Call Array Take on our single chunk
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<ArrayData> new_chunk,
                          TakeAA(current_chunk->data(), indices.data(), options, ctx));
    std::vector<std::shared_ptr<Array>> chunks = {MakeArray(new_chunk)};
    return std::make_shared<ChunkedArray>(std::move(chunks));
  } else {
    // For each index, lookup at which chunk it refers to.
    // We have to do this because the indices are not necessarily sorted.
    // So we can't simply iterate over chunks and pick the slices we need.
    std::vector<Int64Builder> builders(num_chunks);
    std::vector<int64_t> indices_chunks(num_indices);
    const void* indices_raw_data;  // Use Raw data to avoid invoking .Value() on indices
    auto indices_type_id = indices.type()->id();
    switch (indices_type_id) {
      case Type::UINT8:
      case Type::INT8:
        indices_raw_data = static_cast<UInt8Array const&>(indices).raw_values();
        break;
      case Type::UINT16:
      case Type::INT16:
        indices_raw_data = static_cast<UInt16Array const&>(indices).raw_values();
        break;
      case Type::UINT32:
      case Type::INT32:
        indices_raw_data = static_cast<UInt32Array const&>(indices).raw_values();
        break;
      case Type::UINT64:
      case Type::INT64:
        indices_raw_data = static_cast<UInt64Array const&>(indices).raw_values();
        break;
      default:
        DCHECK(false) << "Invalid indices types " << indices.type()->ToString();
        break;
    }

    ChunkResolver index_resolver(values.chunks());
    for (int64_t requested_index = 0; requested_index < num_indices; ++requested_index) {
      uint64_t index;
      switch (indices_type_id) {
        case Type::UINT8:
        case Type::INT8:
          index = static_cast<uint8_t const*>(indices_raw_data)[requested_index];
          break;
        case Type::UINT16:
        case Type::INT16:
          index = static_cast<uint16_t const*>(indices_raw_data)[requested_index];
          break;
        case Type::UINT32:
        case Type::INT32:
          index = static_cast<uint32_t const*>(indices_raw_data)[requested_index];
          break;
        case Type::UINT64:
        case Type::INT64:
          index = static_cast<uint64_t const*>(indices_raw_data)[requested_index];
          break;
        default:
          DCHECK(false) << "Invalid indices types " << indices.type()->ToString();
          break;
      }

      ChunkLocation resolved_index = index_resolver.Resolve(index);
      int64_t chunk_index = resolved_index.chunk_index;
      if (chunk_index >= num_chunks) {
        // ChunkResolver doesn't throw errors when the index is out of bounds
        // it will just return a chunk index that doesn't exist.
        return Status::IndexError("Index ", index, " is out of bounds");
      }
      indices_chunks[requested_index] = chunk_index;
      ARROW_RETURN_NOT_OK(builders[chunk_index].Append(resolved_index.index_in_chunk));
    }

    // Take from the various chunks only the values we actually care about.
    // We first gather all values using Take and then we slice the resulting
    // arrays with the values to create the actual resulting chunks
    // as that is orders of magnitude faster than calling Take multiple times.
    std::vector<std::shared_ptr<arrow::ArrayData>> looked_up_values_data(num_chunks);
    std::vector<arrow::ArraySpan> looked_up_values;
    looked_up_values.reserve(num_chunks);
    for (int i = 0; i < num_chunks; ++i) {
      if (builders[i].length() == 0) {
        // No indices refer to this chunk, so we can skip it
        continue;
      }
      std::shared_ptr<Int64Array> indices_array;
      ARROW_RETURN_NOT_OK(builders[i].Finish(&indices_array));
      ARROW_ASSIGN_OR_RAISE(
        looked_up_values_data[i], TakeAA(values.chunk(i)->data(), indices_array->data(), options, ctx));
      looked_up_values.emplace_back(*looked_up_values_data[i]);
    }

    // Slice the arrays with the values to create the new chunked array out of them
    std::unique_ptr<ArrayBuilder> result_builder;
    ARROW_RETURN_NOT_OK(MakeBuilder(ctx->memory_pool(), values.type(), &result_builder));
    ARROW_RETURN_NOT_OK(result_builder->Reserve(num_indices));
    std::vector<int64_t> consumed_chunk_offset(num_chunks, 0);
    int64_t current_chunk = indices_chunks[0];
    int64_t current_length = 0;
    for (int64_t requested_index = 0; requested_index < num_indices; ++requested_index) {
      int64_t chunk_index = indices_chunks[requested_index];
      if (chunk_index != current_chunk) {
        // Values in previous chunk
        ARROW_RETURN_NOT_OK(result_builder->AppendArraySlice(
            looked_up_values[current_chunk],
            consumed_chunk_offset[current_chunk], current_length));
        consumed_chunk_offset[current_chunk] += current_length;
        current_chunk = chunk_index;
        current_length = 0;
      }
      ++current_length;
    }
    if (current_length > 0) {
      // Remaining values in last chunk
      ARROW_RETURN_NOT_OK(result_builder->AppendArraySlice(
          looked_up_values[current_chunk], consumed_chunk_offset[current_chunk],
          current_length));
    }

    ARROW_ASSIGN_OR_RAISE(auto result_array, result_builder->Finish());
    return std::make_shared<ChunkedArray>(result_array);
  }
}

Result<std::shared_ptr<ChunkedArray>> TakeCC(const ChunkedArray& values,
                                             const ChunkedArray& indices,
                                             const TakeOptions& options,
                                             ExecContext* ctx) {
  auto num_chunks = indices.num_chunks();
  std::vector<std::shared_ptr<Array>> new_chunks(num_chunks);
  for (int i = 0; i < num_chunks; i++) {
    // Take with that indices chunk
    // Note that as currently implemented, this is inefficient because `values`
    // will get concatenated on every iteration of this loop
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<ChunkedArray> current_chunk,
                          TakeCA(values, *indices.chunk(i), options, ctx));
    // Concatenate the result to make a single array for this chunk
    ARROW_ASSIGN_OR_RAISE(new_chunks[i],
                          Concatenate(current_chunk->chunks(), ctx->memory_pool()));
  }
  return std::make_shared<ChunkedArray>(std::move(new_chunks), values.type());
}

Result<std::shared_ptr<ChunkedArray>> TakeAC(const Array& values,
                                             const ChunkedArray& indices,
                                             const TakeOptions& options,
                                             ExecContext* ctx) {
  auto num_chunks = indices.num_chunks();
  std::vector<std::shared_ptr<Array>> new_chunks(num_chunks);
  for (int i = 0; i < num_chunks; i++) {
    // Take with that indices chunk
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<ArrayData> chunk,
                          TakeAA(values.data(), indices.chunk(i)->data(), options, ctx));
    new_chunks[i] = MakeArray(chunk);
  }
  return std::make_shared<ChunkedArray>(std::move(new_chunks), values.type());
}

Result<std::shared_ptr<RecordBatch>> TakeRA(const RecordBatch& batch,
                                            const Array& indices,
                                            const TakeOptions& options,
                                            ExecContext* ctx) {
  auto ncols = batch.num_columns();
  auto nrows = indices.length();
  std::vector<std::shared_ptr<Array>> columns(ncols);
  for (int j = 0; j < ncols; j++) {
    ARROW_ASSIGN_OR_RAISE(std::shared_ptr<ArrayData> col_data,
                          TakeAA(batch.column(j)->data(), indices.data(), options, ctx));
    columns[j] = MakeArray(col_data);
  }
  return RecordBatch::Make(batch.schema(), nrows, std::move(columns));
}

Result<std::shared_ptr<Table>> TakeTA(const Table& table, const Array& indices,
                                      const TakeOptions& options, ExecContext* ctx) {
  auto ncols = table.num_columns();
  std::vector<std::shared_ptr<ChunkedArray>> columns(ncols);

  for (int j = 0; j < ncols; j++) {
    ARROW_ASSIGN_OR_RAISE(columns[j], TakeCA(*table.column(j), indices, options, ctx));
  }
  return Table::Make(table.schema(), std::move(columns));
}

Result<std::shared_ptr<Table>> TakeTC(const Table& table, const ChunkedArray& indices,
                                      const TakeOptions& options, ExecContext* ctx) {
  auto ncols = table.num_columns();
  std::vector<std::shared_ptr<ChunkedArray>> columns(ncols);
  for (int j = 0; j < ncols; j++) {
    ARROW_ASSIGN_OR_RAISE(columns[j], TakeCC(*table.column(j), indices, options, ctx));
  }
  return Table::Make(table.schema(), std::move(columns));
}

const FunctionDoc take_doc(
    "Select values from an input based on indices from another array",
    ("The output is populated with values from the input at positions\n"
     "given by `indices`.  Nulls in `indices` emit null in the output."),
    {"input", "indices"}, "TakeOptions");

// Metafunction for dispatching to different Take implementations other than
// Array-Array.
//
// TODO: Revamp approach to executing Take operations. In addition to being
// overly complex dispatching, there is no parallelization.
class TakeMetaFunction : public MetaFunction {
 public:
  TakeMetaFunction()
      : MetaFunction("take", Arity::Binary(), take_doc, GetDefaultTakeOptions()) {}

  Result<Datum> ExecuteImpl(const std::vector<Datum>& args,
                            const FunctionOptions* options,
                            ExecContext* ctx) const override {
    Datum::Kind index_kind = args[1].kind();
    const auto& take_opts = static_cast<const TakeOptions&>(*options);
    switch (args[0].kind()) {
      case Datum::ARRAY:
        if (index_kind == Datum::ARRAY) {
          return TakeAA(args[0].array(), args[1].array(), take_opts, ctx);
        } else if (index_kind == Datum::CHUNKED_ARRAY) {
          return TakeAC(*args[0].make_array(), *args[1].chunked_array(), take_opts, ctx);
        }
        break;
      case Datum::CHUNKED_ARRAY:
        if (index_kind == Datum::ARRAY) {
          return TakeCA(*args[0].chunked_array(), *args[1].make_array(), take_opts, ctx);
        } else if (index_kind == Datum::CHUNKED_ARRAY) {
          return TakeCC(*args[0].chunked_array(), *args[1].chunked_array(), take_opts,
                        ctx);
        }
        break;
      case Datum::RECORD_BATCH:
        if (index_kind == Datum::ARRAY) {
          return TakeRA(*args[0].record_batch(), *args[1].make_array(), take_opts, ctx);
        }
        break;
      case Datum::TABLE:
        if (index_kind == Datum::ARRAY) {
          return TakeTA(*args[0].table(), *args[1].make_array(), take_opts, ctx);
        } else if (index_kind == Datum::CHUNKED_ARRAY) {
          return TakeTC(*args[0].table(), *args[1].chunked_array(), take_opts, ctx);
        }
        break;
      default:
        break;
    }
    return Status::NotImplemented(
        "Unsupported types for take operation: "
        "values=",
        args[0].ToString(), "indices=", args[1].ToString());
  }
};

// ----------------------------------------------------------------------

}  // namespace

const TakeOptions* GetDefaultTakeOptions() {
  static const auto kDefaultTakeOptions = TakeOptions::Defaults();
  return &kDefaultTakeOptions;
}

std::unique_ptr<Function> MakeTakeMetaFunction() {
  return std::make_unique<TakeMetaFunction>();
}

void PopulateTakeKernels(std::vector<SelectionKernelData>* out) {
  auto take_indices = match::Integer();

  *out = {
      {InputType(match::Primitive()), take_indices, PrimitiveTakeExec},
      {InputType(match::BinaryLike()), take_indices, VarBinaryTakeExec},
      {InputType(match::LargeBinaryLike()), take_indices, LargeVarBinaryTakeExec},
      {InputType(Type::FIXED_SIZE_BINARY), take_indices, FSBTakeExec},
      {InputType(null()), take_indices, NullTakeExec},
      {InputType(Type::DECIMAL128), take_indices, PrimitiveTakeExec},
      {InputType(Type::DECIMAL256), take_indices, PrimitiveTakeExec},
      {InputType(Type::DICTIONARY), take_indices, DictionaryTake},
      {InputType(Type::EXTENSION), take_indices, ExtensionTake},
      {InputType(Type::LIST), take_indices, ListTakeExec},
      {InputType(Type::LARGE_LIST), take_indices, LargeListTakeExec},
      {InputType(Type::FIXED_SIZE_LIST), take_indices, FSLTakeExec},
      {InputType(Type::DENSE_UNION), take_indices, DenseUnionTakeExec},
      {InputType(Type::SPARSE_UNION), take_indices, SparseUnionTakeExec},
      {InputType(Type::STRUCT), take_indices, StructTakeExec},
      {InputType(Type::MAP), take_indices, MapTakeExec},
  };
}

}  // namespace internal
}  // namespace compute
}  // namespace arrow
