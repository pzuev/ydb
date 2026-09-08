#include <ydb/library/yql/dq/comp_nodes/dq_hash_combine_layout.h>

#include <yql/essentials/minikql/mkql_node.h>
#include <yql/essentials/minikql/mkql_node_cast.h>

#include <library/cpp/testing/unittest/registar.h>

#include <bit>
#include <cmath>
#include <limits>
#include <string>

namespace NKikimr::NMiniKQL {
namespace {

using NUdf::TUnboxedValue;
using NUdf::TUnboxedValuePod;

class TLayoutTestEnv {
public:
    TLayoutTestEnv()
        : Alloc(__LOCATION__)
        , Env(Alloc)
    {
    }

    template <typename T>
    TType* Data() {
        return TDataType::Create(NUdf::TDataType<T>::Id, Env);
    }

    TType* Optional(TType* item) {
        return TOptionalType::Create(item, Env);
    }

private:
    TScopedAlloc Alloc;

public:
    TTypeEnvironment Env;
};

class TStorage {
public:
    explicit TStorage(size_t size)
        : Words((size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t))
    {
    }

    void* Data() { return Words.data(); }
    const void* Data() const { return Words.data(); }

private:
    std::vector<std::max_align_t> Words;
};

const TDqHashCombineTupleLayout::TItem& Item(const TDqHashCombineTupleLayout& layout, ui32 logicalIndex) {
    for (const auto& item : layout.GetItems()) {
        if (item.LogicalIndex == logicalIndex) {
            return item;
        }
    }
    Y_ABORT("Missing layout item");
}

template <size_t Alignment>
void TestRecordBoundaries(TArrayRef<TType* const> keyTypes, TArrayRef<TType* const> stateTypes,
    size_t stateOffset, size_t recordSize)
{
    TDqHashCombineRecordLayout<Alignment> layout(keyTypes, stateTypes);
    UNIT_ASSERT_VALUES_EQUAL(layout.GetStateOffset(), stateOffset);
    UNIT_ASSERT_VALUES_EQUAL(layout.GetRecordSize(), recordSize);
}

} // anonymous namespace

Y_UNIT_TEST_SUITE(TDqHashCombineLayoutTest) {
    Y_UNIT_TEST(SectionOffsetsAndAlignment) {
        TLayoutTestEnv env;
        std::vector<TType*> types = {
            env.Data<char*>(),
            env.Data<ui32>(),
            env.Optional(env.Data<double>()),
            env.Data<i64>(),
            env.Data<float>(),
            env.Optional(env.Data<ui64>()),
        };
        TDqHashCombineTupleLayout layout(types);

        UNIT_ASSERT_VALUES_EQUAL(layout.GetUnboxedCount(), 1);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetNative64Count(), 3);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetValidityWordCount(), 1);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetNative32Count(), 2);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetValidityOffset(), 40);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetSize(), 52);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 0).Offset, 0);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 2).Offset, 16);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 3).Offset, 24);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 5).Offset, 32);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 1).Offset, 44);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 4).Offset, 48);

        std::vector<TType*> stateTypes = {env.Data<i32>()};
        TestRecordBoundaries<8>(types, stateTypes, 56, 64);
        TestRecordBoundaries<16>(types, stateTypes, 64, 80);

        std::vector<TType*> empty;
        TestRecordBoundaries<8>(empty, empty, 0, 8);
        TestRecordBoundaries<16>(empty, empty, 0, 16);
    }

    Y_UNIT_TEST(MultipleValidityWords) {
        TLayoutTestEnv env;
        std::vector<TType*> types;
        for (size_t i = 0; i < 33; ++i) {
            types.push_back(env.Optional(env.Data<ui32>()));
        }
        TDqHashCombineTupleLayout layout(types);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetValidityWordCount(), 2);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetValidityOffset(), 0);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetSize(), 140);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 0).Offset, 8);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 32).Offset, 136);
        UNIT_ASSERT_VALUES_EQUAL(Item(layout, 32).ValidityBit, 32);
    }

    Y_UNIT_TEST(RoundTripNativeAndFallbackValues) {
        TLayoutTestEnv env;
        std::vector<TType*> types = {
            env.Data<ui64>(), env.Data<i64>(), env.Data<double>(),
            env.Data<ui32>(), env.Data<i32>(), env.Data<float>(),
            env.Optional(env.Data<ui64>()), env.Optional(env.Data<i64>()), env.Optional(env.Data<double>()),
            env.Optional(env.Data<ui32>()), env.Optional(env.Data<i32>()), env.Optional(env.Data<float>()),
            env.Data<char*>(),
        };
        TDqHashCombineTupleLayout layout(types);
        TStorage storage(layout.GetSize());

        std::vector<TUnboxedValue> values(types.size());
        values[0] = TUnboxedValuePod(std::numeric_limits<ui64>::max());
        values[1] = TUnboxedValuePod(std::numeric_limits<i64>::min());
        values[2] = TUnboxedValuePod(std::numeric_limits<double>::infinity());
        values[3] = TUnboxedValuePod(std::numeric_limits<ui32>::max());
        values[4] = TUnboxedValuePod(std::numeric_limits<i32>::min());
        values[5] = TUnboxedValuePod(-0.0f);
        values[6] = TUnboxedValuePod(ui64{0});
        values[7] = TUnboxedValuePod(i64{-17});
        values[8] = TUnboxedValuePod(-std::numeric_limits<double>::infinity());
        values[9] = TUnboxedValuePod(ui32{42});
        values[10] = TUnboxedValuePod(i32{-42});
        values[11] = TUnboxedValuePod(3.25f);
        values[12] = TUnboxedValuePod(NUdf::TStringValue("a string longer than fourteen bytes"));

        layout.PackMove(values, storage.Data());
        for (const auto& value : values) {
            UNIT_ASSERT(!value.HasValue());
        }

        std::vector<TUnboxedValue> result(types.size());
        layout.UnpackMove(storage.Data(), result);
        UNIT_ASSERT_VALUES_EQUAL(result[0].Get<ui64>(), std::numeric_limits<ui64>::max());
        UNIT_ASSERT_VALUES_EQUAL(result[1].Get<i64>(), std::numeric_limits<i64>::min());
        UNIT_ASSERT(std::isinf(result[2].Get<double>()));
        UNIT_ASSERT_VALUES_EQUAL(result[3].Get<ui32>(), std::numeric_limits<ui32>::max());
        UNIT_ASSERT_VALUES_EQUAL(result[4].Get<i32>(), std::numeric_limits<i32>::min());
        UNIT_ASSERT(std::signbit(result[5].Get<float>()));
        UNIT_ASSERT_VALUES_EQUAL(result[6].Get<ui64>(), 0);
        UNIT_ASSERT_VALUES_EQUAL(result[7].Get<i64>(), -17);
        UNIT_ASSERT(result[8].Get<double>() < 0 && std::isinf(result[8].Get<double>()));
        UNIT_ASSERT_VALUES_EQUAL(result[9].Get<ui32>(), 42);
        UNIT_ASSERT_VALUES_EQUAL(result[10].Get<i32>(), -42);
        UNIT_ASSERT_VALUES_EQUAL(result[11].Get<float>(), 3.25f);
        UNIT_ASSERT_VALUES_EQUAL(std::string(result[12].AsStringRef()), "a string longer than fourteen bytes");

        for (size_t i = 6; i <= 11; ++i) {
            result[i] = TUnboxedValue{};
        }
        layout.PackMove(result, storage.Data());
        layout.UnpackMove(storage.Data(), values);
        for (size_t i = 6; i <= 11; ++i) {
            UNIT_ASSERT(!values[i].HasValue());
        }
    }

    Y_UNIT_TEST(EqualitySemantics) {
        TLayoutTestEnv env;
        std::vector<TType*> types = {
            env.Data<double>(), env.Data<float>(), env.Optional(env.Data<ui64>()), env.Data<char*>()
        };
        TDqHashCombineTupleLayout layout(types);
        TStorage left(layout.GetSize());
        TStorage right(layout.GetSize());

        const double nan1 = std::bit_cast<double>(ui64{0x7ff8000000000001ULL});
        const double nan2 = std::bit_cast<double>(ui64{0x7ff8000000000002ULL});
        std::vector<TUnboxedValue> lhs = {
            TUnboxedValuePod(nan1), TUnboxedValuePod(0.0f), TUnboxedValuePod{},
            TUnboxedValuePod(NUdf::TStringValue("a string longer than fourteen bytes")),
        };
        std::vector<TUnboxedValue> rhs = {
            TUnboxedValuePod(nan2), TUnboxedValuePod(-0.0f), TUnboxedValuePod{}, lhs[3],
        };
        auto pods = [](const std::vector<TUnboxedValue>& values) {
            return TArrayRef<const TUnboxedValuePod>(
                reinterpret_cast<const TUnboxedValuePod*>(values.data()), values.size());
        };

        layout.PackBorrowed(pods(lhs), left.Data());
        layout.PackBorrowed(pods(rhs), right.Data());
        UNIT_ASSERT(layout.Equals(left.Data(), right.Data()));

        rhs[2] = TUnboxedValuePod(ui64{0});
        layout.PackBorrowed(pods(rhs), right.Data());
        UNIT_ASSERT(!layout.Equals(left.Data(), right.Data()));
        rhs[2] = TUnboxedValuePod{};
        rhs[3] = TUnboxedValuePod(NUdf::TStringValue("a different long string value"));
        layout.PackBorrowed(pods(rhs), right.Data());
        UNIT_ASSERT(!layout.Equals(left.Data(), right.Data()));
    }

    Y_UNIT_TEST(OwnershipAndMemoryEstimation) {
        TLayoutTestEnv env;
        std::vector<TType*> types = {env.Data<char*>(), env.Data<ui64>()};
        TDqHashCombineTupleLayout layout(types);
        UNIT_ASSERT_VALUES_EQUAL(layout.GetSize(), 24);
        UNIT_ASSERT(!layout.GetStaticExternalMemorySize());

        TUnboxedValue owner = TUnboxedValuePod(NUdf::TStringValue("a string longer than fourteen bytes"));
        const i32 initialRefCount = owner.RefCount();
        std::vector<TUnboxedValuePod> borrowed = {owner, TUnboxedValuePod(ui64{7})};
        TStorage scratch(layout.GetSize());
        TStorage persistent(layout.GetSize());
        layout.PackBorrowed(borrowed, scratch.Data());
        UNIT_ASSERT_VALUES_EQUAL(owner.RefCount(), initialRefCount);
        layout.CopyWithRefs(scratch.Data(), persistent.Data());
        UNIT_ASSERT_VALUES_EQUAL(owner.RefCount(), initialRefCount + 1);

        const auto memory = layout.EstimateExternalMemorySize(persistent.Data());
        UNIT_ASSERT(memory);
        UNIT_ASSERT_VALUES_EQUAL(*memory, owner.AsStringRef().Size());

        std::vector<TUnboxedValue> copied(types.size());
        layout.UnpackCopy(persistent.Data(), copied);
        UNIT_ASSERT_VALUES_EQUAL(owner.RefCount(), initialRefCount + 2);
        copied.clear();
        UNIT_ASSERT_VALUES_EQUAL(owner.RefCount(), initialRefCount + 1);

        std::vector<TUnboxedValue> moved(types.size());
        layout.UnpackMove(persistent.Data(), moved);
        UNIT_ASSERT_VALUES_EQUAL(owner.RefCount(), initialRefCount + 1);
        UNIT_ASSERT_VALUES_EQUAL(std::string(moved[0].AsStringRef()), std::string(owner.AsStringRef()));
        UNIT_ASSERT_VALUES_EQUAL(moved[1].Get<ui64>(), 7);
        layout.Destroy(persistent.Data());
        moved.clear();
        UNIT_ASSERT_VALUES_EQUAL(owner.RefCount(), initialRefCount);

        std::vector<TType*> empty;
        TDqHashCombineRecordLayout<16> recordLayout(types, empty);
        TStorage record(recordLayout.GetRecordSize());
        recordLayout.Clear(record.Data());
        recordLayout.GetKeyLayout().CopyWithRefs(scratch.Data(), record.Data());
        const auto recordMemory = recordLayout.EstimateMemorySize(record.Data());
        UNIT_ASSERT(recordMemory);
        UNIT_ASSERT_VALUES_EQUAL(*recordMemory, recordLayout.GetRecordSize() + owner.AsStringRef().Size());
        recordLayout.GetKeyLayout().Destroy(record.Data());
    }
}

} // namespace NKikimr::NMiniKQL
