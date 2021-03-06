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

#include "arrow/array/dict_internal.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "arrow/array.h"
#include "arrow/buffer.h"
#include "arrow/status.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/bit_util.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/hashing.h"
#include "arrow/util/int_util.h"
#include "arrow/util/logging.h"
#include "arrow/visitor_inline.h"

namespace arrow {

using internal::checked_cast;
using internal::CopyBitmap;

// ----------------------------------------------------------------------
// DictionaryType unification

template <typename T>
class DictionaryUnifierImpl : public DictionaryUnifier {
 public:
  using ArrayType = typename TypeTraits<T>::ArrayType;
  using DictTraits = typename internal::DictionaryTraits<T>;
  using MemoTableType = typename DictTraits::MemoTableType;

  DictionaryUnifierImpl(MemoryPool* pool, std::shared_ptr<DataType> value_type)
      : pool_(pool), value_type_(value_type), memo_table_(pool) {}

  Status Unify(const Array& dictionary, std::shared_ptr<Buffer>* out) override {
    if (dictionary.null_count() > 0) {
      return Status::Invalid("Cannot yet unify dictionaries with nulls");
    }
    if (!dictionary.type()->Equals(*value_type_)) {
      return Status::Invalid("Dictionary type different from unifier: ",
                             dictionary.type()->ToString());
    }
    const ArrayType& values = checked_cast<const ArrayType&>(dictionary);
    if (out != nullptr) {
      std::shared_ptr<Buffer> result;
      RETURN_NOT_OK(
          AllocateBuffer(pool_, dictionary.length() * sizeof(int32_t), &result));
      auto result_raw = reinterpret_cast<int32_t*>(result->mutable_data());
      for (int64_t i = 0; i < values.length(); ++i) {
        RETURN_NOT_OK(memo_table_.GetOrInsert(values.GetView(i), &result_raw[i]));
      }
      *out = result;
    } else {
      for (int64_t i = 0; i < values.length(); ++i) {
        int32_t unused_memo_index;
        RETURN_NOT_OK(memo_table_.GetOrInsert(values.GetView(i), &unused_memo_index));
      }
    }
    return Status::OK();
  }

  Status Unify(const Array& dictionary) override { return Unify(dictionary, nullptr); }

  Status GetResult(std::shared_ptr<DataType>* out_type,
                   std::shared_ptr<Array>* out_dict) override {
    int64_t dict_length = memo_table_.size();
    std::shared_ptr<DataType> index_type;
    if (dict_length <= std::numeric_limits<int8_t>::max()) {
      index_type = int8();
    } else if (dict_length <= std::numeric_limits<int16_t>::max()) {
      index_type = int16();
    } else if (dict_length <= std::numeric_limits<int32_t>::max()) {
      index_type = int32();
    } else {
      index_type = int64();
    }
    // Build unified dictionary type with the right index type
    *out_type = arrow::dictionary(index_type, value_type_);

    // Build unified dictionary array
    std::shared_ptr<ArrayData> data;
    RETURN_NOT_OK(DictTraits::GetDictionaryArrayData(pool_, value_type_, memo_table_,
                                                     0 /* start_offset */, &data));
    *out_dict = MakeArray(data);
    return Status::OK();
  }

 private:
  MemoryPool* pool_;
  std::shared_ptr<DataType> value_type_;
  MemoTableType memo_table_;
};

struct MakeUnifier {
  MemoryPool* pool;
  std::shared_ptr<DataType> value_type;
  std::unique_ptr<DictionaryUnifier> result;

  MakeUnifier(MemoryPool* pool, std::shared_ptr<DataType> value_type)
      : pool(pool), value_type(value_type) {}

  template <typename T>
  enable_if_no_memoize<T, Status> Visit(const T&) {
    // Default implementation for non-dictionary-supported datatypes
    return Status::NotImplemented("Unification of ", value_type,
                                  " dictionaries is not implemented");
  }

  template <typename T>
  enable_if_memoize<T, Status> Visit(const T&) {
    result.reset(new DictionaryUnifierImpl<T>(pool, value_type));
    return Status::OK();
  }
};

Status DictionaryUnifier::Make(MemoryPool* pool, std::shared_ptr<DataType> value_type,
                               std::unique_ptr<DictionaryUnifier>* out) {
  MakeUnifier maker(pool, value_type);
  RETURN_NOT_OK(VisitTypeInline(*value_type, &maker));
  *out = std::move(maker.result);
  return Status::OK();
}

// ----------------------------------------------------------------------
// DictionaryArray transposition

static bool IsTrivialTransposition(const int32_t* transpose_map,
                                   int64_t input_dict_size) {
  for (int64_t i = 0; i < input_dict_size; ++i) {
    if (transpose_map[i] != i) {
      return false;
    }
  }
  return true;
}

template <typename InType, typename OutType>
static Status TransposeDictIndices(MemoryPool* pool, const ArrayData& in_data,
                                   const int32_t* transpose_map,
                                   const std::shared_ptr<ArrayData>& out_data,
                                   std::shared_ptr<Array>* out) {
  using in_c_type = typename InType::c_type;
  using out_c_type = typename OutType::c_type;
  internal::TransposeInts(in_data.GetValues<in_c_type>(1),
                          out_data->GetMutableValues<out_c_type>(1), in_data.length,
                          transpose_map);
  *out = MakeArray(out_data);
  return Status::OK();
}

Status DictionaryArray::Transpose(MemoryPool* pool, const std::shared_ptr<DataType>& type,
                                  const std::shared_ptr<Array>& dictionary,
                                  const int32_t* transpose_map,
                                  std::shared_ptr<Array>* out) const {
  if (type->id() != Type::DICTIONARY) {
    return Status::TypeError("Expected dictionary type");
  }
  const int64_t in_dict_len = data_->dictionary->length();
  const auto& out_dict_type = checked_cast<const DictionaryType&>(*type);

  const auto& out_index_type =
      static_cast<const FixedWidthType&>(*out_dict_type.index_type());

  auto in_type_id = dict_type_->index_type()->id();
  auto out_type_id = out_index_type.id();

  if (in_type_id == out_type_id && IsTrivialTransposition(transpose_map, in_dict_len)) {
    // Index type and values will be identical => we can simply reuse
    // the existing buffers.
    auto out_data =
        ArrayData::Make(type, data_->length, {data_->buffers[0], data_->buffers[1]},
                        data_->null_count, data_->offset);
    out_data->dictionary = dictionary;
    *out = MakeArray(out_data);
    return Status::OK();
  }

  // Default path: compute a buffer of transposed indices.
  std::shared_ptr<Buffer> out_buffer;
  RETURN_NOT_OK(AllocateBuffer(
      pool, data_->length * out_index_type.bit_width() * CHAR_BIT, &out_buffer));

  // Shift null buffer if the original offset is non-zero
  std::shared_ptr<Buffer> null_bitmap;
  if (data_->offset != 0) {
    ARROW_ASSIGN_OR_RAISE(
        null_bitmap, CopyBitmap(pool, null_bitmap_data_, data_->offset, data_->length));
  } else {
    null_bitmap = data_->buffers[0];
  }

  auto out_data =
      ArrayData::Make(type, data_->length, {null_bitmap, out_buffer}, data_->null_count);
  out_data->dictionary = dictionary;

#define TRANSPOSE_IN_OUT_CASE(IN_INDEX_TYPE, OUT_INDEX_TYPE)    \
  case OUT_INDEX_TYPE::type_id:                                 \
    return TransposeDictIndices<IN_INDEX_TYPE, OUT_INDEX_TYPE>( \
        pool, *data_, transpose_map, out_data, out);

#define TRANSPOSE_IN_CASE(IN_INDEX_TYPE)                        \
  case IN_INDEX_TYPE::type_id:                                  \
    switch (out_type_id) {                                      \
      TRANSPOSE_IN_OUT_CASE(IN_INDEX_TYPE, Int8Type)            \
      TRANSPOSE_IN_OUT_CASE(IN_INDEX_TYPE, Int16Type)           \
      TRANSPOSE_IN_OUT_CASE(IN_INDEX_TYPE, Int32Type)           \
      TRANSPOSE_IN_OUT_CASE(IN_INDEX_TYPE, Int64Type)           \
      default:                                                  \
        return Status::NotImplemented("unexpected index type"); \
    }

  switch (in_type_id) {
    TRANSPOSE_IN_CASE(Int8Type)
    TRANSPOSE_IN_CASE(Int16Type)
    TRANSPOSE_IN_CASE(Int32Type)
    TRANSPOSE_IN_CASE(Int64Type)
    default:
      return Status::NotImplemented("unexpected index type");
  }
#undef TRANSPOSE_IN_CASE
#undef TRANSPOSE_IN_OUT_CASE
}

}  // namespace arrow
