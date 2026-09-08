#include "dq_hash_combine_layout.h"

#include <yql/essentials/minikql/computation/mkql_computation_node_holders.h>
#include <yql/essentials/minikql/mkql_node_cast.h>
#include <yql/essentials/public/udf/udf_type_ops.h>

#include <util/system/unaligned_mem.h>
#include <util/system/yassert.h>

#include <cstring>

namespace NKikimr::NMiniKQL {
namespace {

using NUdf::EDataSlot;
using NUdf::TUnboxedValue;
using NUdf::TUnboxedValuePod;

TDqHashCombineTupleLayout::EStorage GetStorage(EDataSlot slot) {
    switch (slot) {
        case EDataSlot::Uint64:
        case EDataSlot::Int64:
        case EDataSlot::Double:
            return TDqHashCombineTupleLayout::EStorage::Native64;
        case EDataSlot::Uint32:
        case EDataSlot::Int32:
        case EDataSlot::Float:
            return TDqHashCombineTupleLayout::EStorage::Native32;
        default:
            return TDqHashCombineTupleLayout::EStorage::Unboxed;
    }
}

template <typename T>
void PackNative(const TUnboxedValuePod& value, void* storage, size_t offset) {
    WriteUnaligned<T>(static_cast<char*>(storage) + offset, value.Get<T>());
}

void PackNative(EDataSlot slot, const TUnboxedValuePod& value, void* storage, size_t offset) {
    switch (slot) {
        case EDataSlot::Uint64: PackNative<ui64>(value, storage, offset); break;
        case EDataSlot::Int64: PackNative<i64>(value, storage, offset); break;
        case EDataSlot::Double: PackNative<double>(value, storage, offset); break;
        case EDataSlot::Uint32: PackNative<ui32>(value, storage, offset); break;
        case EDataSlot::Int32: PackNative<i32>(value, storage, offset); break;
        case EDataSlot::Float: PackNative<float>(value, storage, offset); break;
        default: Y_ABORT("Unexpected native data slot");
    }
}

template <typename T>
TUnboxedValuePod UnpackNative(const void* storage, size_t offset) {
    return TUnboxedValuePod(ReadUnaligned<T>(static_cast<const char*>(storage) + offset));
}

TUnboxedValuePod UnpackNative(EDataSlot slot, const void* storage, size_t offset) {
    switch (slot) {
        case EDataSlot::Uint64: return UnpackNative<ui64>(storage, offset);
        case EDataSlot::Int64: return UnpackNative<i64>(storage, offset);
        case EDataSlot::Double: return UnpackNative<double>(storage, offset);
        case EDataSlot::Uint32: return UnpackNative<ui32>(storage, offset);
        case EDataSlot::Int32: return UnpackNative<i32>(storage, offset);
        case EDataSlot::Float: return UnpackNative<float>(storage, offset);
        default: Y_ABORT("Unexpected native data slot");
    }
}

template <typename T>
bool EqualNative(const void* left, const void* right, size_t offset) {
    return ReadUnaligned<T>(static_cast<const char*>(left) + offset) ==
        ReadUnaligned<T>(static_cast<const char*>(right) + offset);
}

template <typename T>
bool EqualNativeFloat(const void* left, const void* right, size_t offset) {
    const TUnboxedValuePod lhs(ReadUnaligned<T>(static_cast<const char*>(left) + offset));
    const TUnboxedValuePod rhs(ReadUnaligned<T>(static_cast<const char*>(right) + offset));
    return NUdf::EquateFloats<T>(lhs, rhs);
}

bool EqualNative(EDataSlot slot, const void* left, const void* right, size_t offset) {
    switch (slot) {
        case EDataSlot::Uint64: return EqualNative<ui64>(left, right, offset);
        case EDataSlot::Int64: return EqualNative<i64>(left, right, offset);
        case EDataSlot::Double: return EqualNativeFloat<double>(left, right, offset);
        case EDataSlot::Uint32: return EqualNative<ui32>(left, right, offset);
        case EDataSlot::Int32: return EqualNative<i32>(left, right, offset);
        case EDataSlot::Float: return EqualNativeFloat<float>(left, right, offset);
        default: Y_ABORT("Unexpected native data slot");
    }
}

bool EqualFallback(TType* type, const TUnboxedValuePod& left, const TUnboxedValuePod& right) {
    if (type->IsOptional()) {
        if (!left || !right) {
            return bool(left) == bool(right);
        }
        TType* itemType = AS_TYPE(TOptionalType, type)->GetItemType();
        if (itemType->IsOptional()) {
            return EqualFallback(itemType, left.GetOptionalValue(), right.GetOptionalValue());
        }
        return EqualFallback(itemType, left, right);
    }
    if (type->IsData()) {
        const auto slot = AS_TYPE(TDataType, type)->GetDataSlot();
        MKQL_ENSURE(slot, "DqHashCombine key item has no data slot");
        return NUdf::EquateValues(*slot, left, right);
    }
    if (type->IsTuple()) {
        const auto elements = AS_TYPE(TTupleType, type)->GetElements();
        for (size_t i = 0; i < elements.size(); ++i) {
            if (!EqualFallback(elements[i], left.GetElement(i), right.GetElement(i))) {
                return false;
            }
        }
        return true;
    }
    MKQL_ENSURE(false, "Unsupported fallback key type in DqHashCombine");
}

std::optional<size_t> GetStaticUvSizeBound(TType* type) {
    if (type->IsOptional()) {
        return GetStaticUvSizeBound(AS_TYPE(TOptionalType, type)->GetItemType());
    }
    if (type->IsData()) {
        const auto slot = AS_TYPE(TDataType, type)->GetDataSlot();
        if (!slot) {
            return {};
        }
        switch (*slot) {
            case EDataSlot::DyNumber:
            case EDataSlot::Json:
            case EDataSlot::JsonDocument:
            case EDataSlot::Yson:
            case EDataSlot::Utf8:
            case EDataSlot::String:
                return {};
            default:
                return sizeof(TUnboxedValuePod);
        }
    }
    if (type->IsTuple()) {
        size_t result = sizeof(TUnboxedValuePod) + sizeof(TDirectArrayHolderInplace);
        for (TType* item : AS_TYPE(TTupleType, type)->GetElements()) {
            const auto itemSize = GetStaticUvSizeBound(item);
            if (!itemSize) {
                return {};
            }
            result += *itemSize;
        }
        return result;
    }
    return {};
}

std::optional<size_t> EstimateUvSize(const TUnboxedValuePod& value, TType* type) {
    if (!value.HasValue() || value.IsEmbedded() || value.IsInvalid()) {
        return sizeof(TUnboxedValuePod);
    }
    if (value.IsString()) {
        return sizeof(TUnboxedValuePod) + value.AsStringRef().Size();
    }
    if (!value.IsBoxed()) {
        return {};
    }
    while (type->IsOptional()) {
        type = AS_TYPE(TOptionalType, type)->GetItemType();
    }
    if (!type->IsTuple()) {
        return {};
    }

    const auto elements = AS_TYPE(TTupleType, type)->GetElements();
    size_t result = sizeof(TUnboxedValuePod) + sizeof(TDirectArrayHolderInplace);
    for (size_t i = 0; i < elements.size(); ++i) {
        const auto itemSize = EstimateUvSize(value.GetElement(i), elements[i]);
        if (!itemSize) {
            return {};
        }
        result += *itemSize;
    }
    return result;
}

} // anonymous namespace

TDqHashCombineTupleLayout::TDqHashCombineTupleLayout(TArrayRef<TType* const> types)
    : Types(types.begin(), types.end())
{
    Items.reserve(types.size());
    ui32 optionalNativeCount = 0;

    for (ui32 i = 0; i < types.size(); ++i) {
        TType* type = types[i];
        bool optional = false;
        bool nestedOptional = false;
        if (type->IsOptional()) {
            optional = true;
            type = AS_TYPE(TOptionalType, type)->GetItemType();
            nestedOptional = type->IsOptional();
        }

        TType* unpackedType = type;
        while (unpackedType->IsOptional()) {
            unpackedType = AS_TYPE(TOptionalType, unpackedType)->GetItemType();
        }
        std::optional<EDataSlot> slot;
        if (unpackedType->IsData()) {
            const auto dataSlot = AS_TYPE(TDataType, unpackedType)->GetDataSlot();
            if (dataSlot) {
                slot = dataSlot.GetRef();
            }
        }

        EStorage storage = !nestedOptional && type->IsData() && slot ? GetStorage(*slot) : EStorage::Unboxed;
        TItem item{
            .LogicalIndex = i,
            .Offset = 0,
            .DataSlot = slot,
            .Storage = storage,
            .Optional = optional,
        };
        switch (storage) {
            case EStorage::Unboxed: ++UnboxedCount; break;
            case EStorage::Native64: ++Native64Count; break;
            case EStorage::Native32: ++Native32Count; break;
        }
        if (optional && storage != EStorage::Unboxed) {
            item.ValidityBit = optionalNativeCount++;
        }
        Items.push_back(item);
    }

    std::stable_sort(Items.begin(), Items.end(), [](const TItem& left, const TItem& right) {
        return left.Storage < right.Storage;
    });

    ValidityWordCount = (optionalNativeCount + 31) / 32;
    const size_t native64Offset = UnboxedCount * sizeof(TUnboxedValuePod);
    ValidityOffset = native64Offset + Native64Count * sizeof(ui64);
    const size_t native32Offset = ValidityOffset + ValidityWordCount * sizeof(ui32);
    Size = native32Offset + Native32Count * sizeof(ui32);

    size_t unboxed = 0;
    size_t native64 = 0;
    size_t native32 = 0;
    for (auto& item : Items) {
        switch (item.Storage) {
            case EStorage::Unboxed:
                item.Offset = unboxed++ * sizeof(TUnboxedValuePod);
                break;
            case EStorage::Native64:
                item.Offset = native64Offset + native64++ * sizeof(ui64);
                break;
            case EStorage::Native32:
                item.Offset = native32Offset + native32++ * sizeof(ui32);
                break;
        }
    }
}

bool TDqHashCombineTupleLayout::IsPresent(const void* storage, const TItem& item) const {
    if (item.ValidityBit == TItem::NoValidityBit) {
        return true;
    }
    const ui32 word = ReadUnaligned<ui32>(static_cast<const char*>(storage) + ValidityOffset +
        sizeof(ui32) * (item.ValidityBit / 32));
    return word & (ui32{1} << (item.ValidityBit % 32));
}

void TDqHashCombineTupleLayout::SetPresent(void* storage, const TItem& item) const {
    char* wordPtr = static_cast<char*>(storage) + ValidityOffset + sizeof(ui32) * (item.ValidityBit / 32);
    const ui32 word = ReadUnaligned<ui32>(wordPtr) | (ui32{1} << (item.ValidityBit % 32));
    WriteUnaligned<ui32>(wordPtr, word);
}

void TDqHashCombineTupleLayout::PackBorrowed(TArrayRef<const TUnboxedValuePod> values, void* storage) const {
    Y_ABORT_UNLESS(values.size() == Items.size());
    Clear(storage);
    for (const auto& item : Items) {
        const auto& value = values[item.LogicalIndex];
        if (item.Storage == EStorage::Unboxed) {
            *reinterpret_cast<TUnboxedValuePod*>(static_cast<char*>(storage) + item.Offset) = value;
        } else if (!item.Optional || value.HasValue()) {
            if (item.Optional) {
                SetPresent(storage, item);
            }
            PackNative(*item.DataSlot, value, storage, item.Offset);
        }
    }
}

void TDqHashCombineTupleLayout::CopyWithRefs(const void* source, void* destination) const {
    std::memcpy(destination, source, Size);
    for (const auto& item : Items) {
        if (item.Storage == EStorage::Unboxed) {
            reinterpret_cast<TUnboxedValuePod*>(static_cast<char*>(destination) + item.Offset)->Ref();
        }
    }
}

void TDqHashCombineTupleLayout::PackMove(TArrayRef<TUnboxedValue> values, void* storage) const {
    Y_ABORT_UNLESS(values.size() == Items.size());
    Clear(storage);
    for (const auto& item : Items) {
        auto& value = values[item.LogicalIndex];
        auto& pod = static_cast<TUnboxedValuePod&>(value);
        if (item.Storage == EStorage::Unboxed) {
            *reinterpret_cast<TUnboxedValuePod*>(static_cast<char*>(storage) + item.Offset) = pod;
        } else if (!item.Optional || value.HasValue()) {
            if (item.Optional) {
                SetPresent(storage, item);
            }
            PackNative(*item.DataSlot, value, storage, item.Offset);
        }
        pod = TUnboxedValuePod{};
    }
}

void TDqHashCombineTupleLayout::UnpackCopy(const void* storage, TArrayRef<TUnboxedValue> values) const {
    Y_ABORT_UNLESS(values.size() == Items.size());
    for (const auto& item : Items) {
        if (item.Storage == EStorage::Unboxed) {
            values[item.LogicalIndex] = *reinterpret_cast<const TUnboxedValuePod*>(
                static_cast<const char*>(storage) + item.Offset);
        } else if (!item.Optional || IsPresent(storage, item)) {
            values[item.LogicalIndex] = UnpackNative(*item.DataSlot, storage, item.Offset);
        } else {
            values[item.LogicalIndex] = TUnboxedValue{};
        }
    }
}

void TDqHashCombineTupleLayout::UnpackMove(void* storage, TArrayRef<TUnboxedValue> values) const {
    Y_ABORT_UNLESS(values.size() == Items.size());
    for (const auto& item : Items) {
        values[item.LogicalIndex] = TUnboxedValue{};
        if (item.Storage == EStorage::Unboxed) {
            auto* source = reinterpret_cast<TUnboxedValuePod*>(static_cast<char*>(storage) + item.Offset);
            static_cast<TUnboxedValuePod&>(values[item.LogicalIndex]) = *source;
            *source = TUnboxedValuePod{};
        } else if (!item.Optional || IsPresent(storage, item)) {
            values[item.LogicalIndex] = UnpackNative(*item.DataSlot, storage, item.Offset);
        }
    }
    Clear(storage);
}

void TDqHashCombineTupleLayout::Clear(void* storage) const {
    std::memset(storage, 0, Size);
}

void TDqHashCombineTupleLayout::Destroy(void* storage) const {
    for (const auto& item : Items) {
        if (item.Storage == EStorage::Unboxed) {
            auto* value = reinterpret_cast<TUnboxedValuePod*>(static_cast<char*>(storage) + item.Offset);
            value->UnRef();
            *value = TUnboxedValuePod{};
        }
    }
}

bool TDqHashCombineTupleLayout::Equals(const void* left, const void* right) const {
    for (const auto& item : Items) {
        if (item.Storage == EStorage::Unboxed) {
            const auto& lhs = *reinterpret_cast<const TUnboxedValuePod*>(static_cast<const char*>(left) + item.Offset);
            const auto& rhs = *reinterpret_cast<const TUnboxedValuePod*>(static_cast<const char*>(right) + item.Offset);
            if (!EqualFallback(Types[item.LogicalIndex], lhs, rhs)) {
                return false;
            }
            continue;
        }

        if (item.Optional) {
            const bool lhsPresent = IsPresent(left, item);
            const bool rhsPresent = IsPresent(right, item);
            if (lhsPresent != rhsPresent) {
                return false;
            }
            if (!lhsPresent) {
                continue;
            }
        }
        if (!EqualNative(*item.DataSlot, left, right, item.Offset)) {
            return false;
        }
    }
    return true;
}

std::optional<size_t> TDqHashCombineTupleLayout::GetStaticExternalMemorySize() const {
    size_t result = 0;
    for (const auto& item : Items) {
        if (item.Storage != EStorage::Unboxed) {
            continue;
        }
        const auto size = GetStaticUvSizeBound(Types[item.LogicalIndex]);
        if (!size) {
            return {};
        }
        result += *size - sizeof(TUnboxedValuePod);
    }
    return result;
}

std::optional<size_t> TDqHashCombineTupleLayout::EstimateExternalMemorySize(const void* storage) const {
    size_t result = 0;
    for (const auto& item : Items) {
        if (item.Storage != EStorage::Unboxed) {
            continue;
        }
        const auto& value = *reinterpret_cast<const TUnboxedValuePod*>(static_cast<const char*>(storage) + item.Offset);
        const auto size = EstimateUvSize(value, Types[item.LogicalIndex]);
        if (!size) {
            return {};
        }
        result += *size - sizeof(TUnboxedValuePod);
    }
    return result;
}

} // namespace NKikimr::NMiniKQL
