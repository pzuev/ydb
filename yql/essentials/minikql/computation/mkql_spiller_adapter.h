#pragma once
#include "mkql_spiller.h"
#include <yql/essentials/minikql/computation/mkql_computation_node_pack.h>
#include <contrib/libs/lz4/lz4.h>
#include <util/stream/buffer.h>

#include <utility>

namespace NKikimr::NMiniKQL {

/// Stores and loads very long sequences of TMultiType UVs
/// Can split sequences into chunks
/// Sends chunks to ISplitter and keeps assigned keys
/// When all data is written switches to read mode. Switching back to writing mode is not supported
/// Provides an interface for sequential read (like forward iterator)
/// When interaction with ISpiller is required, Write and Read operations return a Future
class TWideUnboxedValuesSpillerAdapter {
public:
    TWideUnboxedValuesSpillerAdapter(ISpiller::TPtr spiller, const TMultiType* type, size_t sizeLimit, ui64 minMemorySizeToReport = 10_KB)
        : Spiller_(std::move(spiller))
        , ItemType_(type)
        , SizeLimit_(sizeLimit)
        , Packer_(type, EValuePackerVersion::V1)
        , MinMemorySizeToReport_(minMemorySizeToReport)
    {
    }

    /// Write wide UV item
    /// \returns
    ///  - nullopt, if thee values are accumulated
    ///  - TFeature, if the values are being stored asynchronously and a caller must wait until async operation ends
    ///    In this case a caller must wait operation completion and call StoreCompleted.
    ///    Design note: not using Subscribe on a Future here to avoid possible race condition
    std::optional<NThreading::TFuture<ISpiller::TKey>> WriteWideItem(const TArrayRef<NUdf::TUnboxedValuePod>& wideItem) {
        Packer_.AddWideItem(wideItem.data(), wideItem.size());
        return FlushPacker(false);
    }

    std::optional<NThreading::TFuture<ISpiller::TKey>> FinishWriting() {
        return FlushPacker(true);
    }

    void AsyncWriteCompleted(ISpiller::TKey key) {
        StoredChunks_.push_back(key);
        ReportPackerFreed();
    }

    // Extracting interface
    bool Empty() const {
        return StoredChunks_.empty() && !CurrentBatch_;
    }
    std::optional<NThreading::TFuture<std::optional<NYql::TChunkedBuffer>>> ExtractWideItem(const TArrayRef<NUdf::TUnboxedValue>& wideItem) {
        MKQL_ENSURE(!Empty(), "Internal logic error");
        if (CurrentBatch_) {
            auto row = CurrentBatch_->Head();
            for (size_t i = 0; i != wideItem.size(); ++i) {
                wideItem[i] = row[i];
            }
            CurrentBatch_->Pop();
            if (CurrentBatch_->empty()) {
                CurrentBatch_ = std::nullopt;
            }
            return std::nullopt;
        } else {
            auto r = Spiller_->Get(StoredChunks_.front());
            StoredChunks_.pop_front();
            return r;
        }
    }

    void AsyncReadCompleted(NYql::TChunkedBuffer&& rope, const THolderFactory& holderFactory) {
        TUnboxedValueBatch batch(ItemType_);
        size_t linearSize = rope.Size();
        TBufferOutput rawCompressed(linearSize);
        rope.CopyTo(rawCompressed, linearSize);

        if (linearSize < 4) {
            ythrow yexception() << "lz4 chunk missing" << Endl;
        }

        int uncompressedSize = ReadUnaligned<int>(rawCompressed.Buffer().Data());
        if (uncompressedSize < 4) {
            ythrow yexception() << "lz4 chunk corrupted" << Endl;
        }

        TString decomp;
        decomp.resize(uncompressedSize, 0);

        int decompResult = LZ4_decompress_safe(rawCompressed.Buffer().Data() + sizeof(int), decomp.begin(), linearSize - 4, uncompressedSize);
        if (decompResult < uncompressedSize) {
            ythrow yexception() << "lz4 decompression failed: " << linearSize << " -> " << uncompressedSize;
        }

        ui32 magic = ReadUnaligned<ui32>(decomp.begin() + decomp.size() - 4);
        if (magic != 0xC0DED00D) {
            ythrow yexception() << "lz4: magic failed" << Endl;
        }
        decomp.resize(decomp.size() - 4, 0);

        NYql::TChunkedBuffer destRope;
        destRope.Append(std::move(decomp));

        Packer_.UnpackBatch(std::move(destRope), holderFactory, batch);
        CurrentBatch_ = std::move(batch);
    }

private:
    void ReportPackerSize(ui64 currentSize, bool forced) {
        if (currentSize < ReportedPackerSize_) {
            Y_DEBUG_ABORT("Packer size should always grow");
            return;
        }
        ui64 sizeDiff = currentSize - ReportedPackerSize_;
        if (sizeDiff > MinMemorySizeToReport_ || forced) {
            Spiller_->ReportAlloc(sizeDiff);
            ReportedPackerSize_ += sizeDiff;
        }
    }

    void ReportPackerFreed() {
        Spiller_->ReportFree(ReportedPackerSize_);
        ReportedPackerSize_ = 0;
    }

    std::optional<NThreading::TFuture<ISpiller::TKey>> FlushPacker(bool forced) {
        if (Packer_.IsEmpty()) {
            return std::nullopt;
        }

        ui64 estimatedPackedSize = Packer_.PackedSizeEstimate();
        ReportPackerSize(estimatedPackedSize, forced);
        if (estimatedPackedSize > SizeLimit_ || forced) {
            auto chunkedBuffer = Packer_.Finish();
            size_t sourceDataSize = chunkedBuffer.Size();

            TBufferOutput linearUncompressed(sourceDataSize + 4);
            chunkedBuffer.CopyTo(linearUncompressed);
            linearUncompressed.Buffer().Advance(4);
            WriteUnaligned<ui32>(linearUncompressed.Buffer().Begin() + sourceDataSize, 0xC0DED00D);

            if (linearUncompressed.Buffer().Size() > std::numeric_limits<int>::max() - 32) {
                ythrow yexception() << "chunk to compress is too large" << Endl;
            }
            int srcSize = static_cast<int>(linearUncompressed.Buffer().Size());

            int destSizeEst = LZ4_compressBound(srcSize);

            TString outComp;
            outComp.resize(destSizeEst + 4, 0);
            int compressedBytes = LZ4_compress_default(linearUncompressed.Buffer().Data(), outComp.begin() + 4, srcSize, destSizeEst);
            if (!compressedBytes) {
                ythrow yexception() << "compression failed" << Endl;
            }
            WriteUnaligned<int>(outComp.begin(), srcSize);
            outComp.resize(compressedBytes + 4, 0);

            NYql::TChunkedBuffer toSpill;
            toSpill.Append(std::move(outComp));

            return Spiller_->Put(std::move(toSpill));
        }

        return std::nullopt;
    }

    ISpiller::TPtr Spiller_;
    const TMultiType* const ItemType_;
    const size_t SizeLimit_;
    TValuePackerTransport<false> Packer_;
    std::deque<ISpiller::TKey> StoredChunks_;
    std::optional<TUnboxedValueBatch> CurrentBatch_;
    ui64 ReportedPackerSize_ = 0;
    const ui64 MinMemorySizeToReport_;
};

} // namespace NKikimr::NMiniKQL
