#pragma once

#include <yql/essentials/minikql/mkql_node.h>
#include <yql/essentials/public/udf/udf_value.h>

#include <util/generic/array_ref.h>
#include <util/system/types.h>

#include <algorithm>
#include <limits>
#include <optional>
#include <vector>

namespace NKikimr::NMiniKQL {

class TDqHashCombineTupleLayout {
public:
    enum class EStorage : ui8 {
        Unboxed,
        Native64,
        Native32,
    };

    struct TItem {
        static constexpr ui32 NoValidityBit = std::numeric_limits<ui32>::max();

        ui32 LogicalIndex;
        ui32 Offset;
        std::optional<NUdf::EDataSlot> DataSlot;
        EStorage Storage;
        ui32 ValidityBit = NoValidityBit;
        bool Optional = false;
    };

    explicit TDqHashCombineTupleLayout(TArrayRef<TType* const> types);

    size_t GetSize() const noexcept { return Size; }
    size_t GetUnboxedCount() const noexcept { return UnboxedCount; }
    size_t GetNative64Count() const noexcept { return Native64Count; }
    size_t GetNative32Count() const noexcept { return Native32Count; }
    size_t GetValidityWordCount() const noexcept { return ValidityWordCount; }
    size_t GetValidityOffset() const noexcept { return ValidityOffset; }
    const std::vector<TItem>& GetItems() const noexcept { return Items; }

    void PackBorrowed(TArrayRef<const NUdf::TUnboxedValuePod> values, void* storage) const;
    void CopyWithRefs(const void* source, void* destination) const;
    void PackMove(TArrayRef<NUdf::TUnboxedValue> values, void* storage) const;
    void UnpackCopy(const void* storage, TArrayRef<NUdf::TUnboxedValue> values) const;
    void UnpackMove(void* storage, TArrayRef<NUdf::TUnboxedValue> values) const;
    void Clear(void* storage) const;
    void Destroy(void* storage) const;

    bool Equals(const void* left, const void* right) const;

    std::optional<size_t> GetStaticExternalMemorySize() const;
    std::optional<size_t> EstimateExternalMemorySize(const void* storage) const;

private:
    bool IsPresent(const void* storage, const TItem& item) const;
    void SetPresent(void* storage, const TItem& item) const;

    std::vector<TItem> Items;
    std::vector<TType*> Types;
    size_t Size = 0;
    size_t UnboxedCount = 0;
    size_t Native64Count = 0;
    size_t Native32Count = 0;
    size_t ValidityWordCount = 0;
    size_t ValidityOffset = 0;
};

template <size_t Alignment>
class TDqHashCombineRecordLayout {
public:
    static_assert(Alignment == 8 || Alignment == 16);

    TDqHashCombineRecordLayout(TArrayRef<TType* const> keyTypes, TArrayRef<TType* const> stateTypes)
        : KeyLayout(keyTypes)
        , StateLayout(stateTypes)
        , StateOffset(AlignUp(KeyLayout.GetSize()))
        , RecordSize(std::max(Alignment, AlignUp(StateOffset + StateLayout.GetSize())))
    {
    }

    static constexpr size_t GetAlignment() noexcept { return Alignment; }
    const TDqHashCombineTupleLayout& GetKeyLayout() const noexcept { return KeyLayout; }
    const TDqHashCombineTupleLayout& GetStateLayout() const noexcept { return StateLayout; }
    size_t GetStateOffset() const noexcept { return StateOffset; }
    size_t GetRecordSize() const noexcept { return RecordSize; }

    void Clear(void* record) const {
        __builtin_memset(record, 0, RecordSize);
    }

    std::optional<size_t> GetStaticMemorySize() const {
        const auto key = KeyLayout.GetStaticExternalMemorySize();
        const auto state = StateLayout.GetStaticExternalMemorySize();
        if (!key || !state) {
            return {};
        }
        return RecordSize + *key + *state;
    }

    std::optional<size_t> EstimateMemorySize(const void* record) const {
        const auto key = KeyLayout.EstimateExternalMemorySize(record);
        const auto state = StateLayout.EstimateExternalMemorySize(
            static_cast<const char*>(record) + StateOffset);
        if (!key || !state) {
            return {};
        }
        return RecordSize + *key + *state;
    }

private:
    static constexpr size_t AlignUp(size_t value) noexcept {
        return (value + Alignment - 1) & ~(Alignment - 1);
    }

    TDqHashCombineTupleLayout KeyLayout;
    TDqHashCombineTupleLayout StateLayout;
    size_t StateOffset;
    size_t RecordSize;
};

#ifndef YDB_DQ_HASH_COMBINE_RECORD_ALIGNMENT
#define YDB_DQ_HASH_COMBINE_RECORD_ALIGNMENT 8
#endif

using TDqHashCombineLayout = TDqHashCombineRecordLayout<YDB_DQ_HASH_COMBINE_RECORD_ALIGNMENT>;

struct TDqHashCombinePackedEqual {
    explicit TDqHashCombinePackedEqual(const TDqHashCombineTupleLayout* layout)
        : Layout(layout)
    {
    }

    bool operator()(const char* left, const char* right) const {
        return Layout->Equals(left, right);
    }

    const TDqHashCombineTupleLayout* Layout;
};

} // namespace NKikimr::NMiniKQL
