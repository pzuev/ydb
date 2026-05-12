#include "timing_trace.h"

#include <util/generic/vector.h>
#include <util/string/subst.h>

namespace TTimingTrace {

TTraceRecorder& GetRecorder()
{
    return *Singleton<TTraceRecorder>();
}

TTraceRecorder::TTraceRecorder()
    : CurrentBatch()
{
    CurrentBatch.reserve(BATCH_SIZE);
}

TTraceRecorder::~TTraceRecorder() {
    Stop();
}

void TTraceRecorder::Start() {
    if (!WorkerThread) {
        Stopped.store(false);
        WorkerThread = std::make_unique<TThread>([this]() { this->WorkerThreadFunc(); });
        WorkerThread->Start();
    }
}

void TTraceRecorder::Stop() {
    if (WorkerThread) {
        Stopped.store(true);
        WorkerThread->Join();
        WorkerThread.reset();
    }
}

void TTraceRecorder::Record(TTraceEntry&& entry) {
    TGuard<TMutex> guard(RecordsMutex);
    CurrentBatch.push_back(std::move(entry));
}

void TTraceRecorder::WorkerThreadFunc() {
    TFile outFile("timing_trace.tsv", OpenAlways|ForAppend);
    TFileOutput outStream(outFile);

    std::vector<TTraceEntry> batchToProcess;
    batchToProcess.reserve(BATCH_SIZE);

    bool exitSignal = false;

    while (true) {
        Sleep(TDuration::MilliSeconds(200));

        exitSignal = Stopped.load();

        // Take a lock and move everything from the current accumulated batch
        {
            TGuard<TMutex> guard(RecordsMutex);
            if (!CurrentBatch.empty()) {
                batchToProcess.swap(CurrentBatch);
                CurrentBatch.reserve(BATCH_SIZE);  // Reserve for next batch
            }
        }

        // Write data to the outStream, if any
        for (const auto& record : batchToProcess) {
            outStream << record.Start.MicroSeconds() << "\t"
                      << record.End.MicroSeconds() << "\t"
                      << record.LaneName << "\t"
                      << record.Text << "\n";
        }

        // Flush data
        if (!batchToProcess.empty()) {
            outStream.Flush();
            batchToProcess.clear();
        }

        if (exitSignal) {
            break;
        }
    }
}

// RAII-style tracing wrapper
TTraceScope::TTraceScope(const TString& laneName, const TString& text)
    : LaneName(laneName)
    , Text(text)
    , Start(TInstant::Now())
{
}

TTraceScope::~TTraceScope() {
    TTraceEntry entry;
    entry.Start = Start;
    entry.End = TInstant::Now();
    entry.LaneName = LaneName;
    entry.Text = Text;

    GetRecorder().Record(std::move(entry));
}

}