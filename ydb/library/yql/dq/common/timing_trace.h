#pragma once
#include <util/datetime/base.h>
#include <util/generic/singleton.h>
#include <util/generic/string.h>
#include <util/system/thread.h>
#include <util/stream/file.h>
#include <util/system/mutex.h>
#include <util/system/condvar.h>
#include <util/system/guard.h>

namespace TTimingTrace {

struct TTraceEntry {
    TInstant Start; // Time span
    TInstant End;
    TString LaneName; // Process/thread/actor name; assume it doesn't need escaping in the output
    TString Text; // Span description to be displayed in the trace viewer.
};


class TTraceRecorder {
public:
    TTraceRecorder();
    ~TTraceRecorder();

    void Start();
    void Stop();
    void Record(TTraceEntry&& entry);

private:
    void WorkerThreadFunc();

    static constexpr size_t BATCH_SIZE = 10000;

    TMutex RecordsMutex;
    std::vector<TTraceEntry> CurrentBatch;
    std::atomic<bool> Stopped = false;
    std::unique_ptr<TThread> WorkerThread;
};

class TTraceScope {
public:
    TTraceScope(const TString& laneName, const TString& text);
    ~TTraceScope();

private:
    TString LaneName;
    TString Text;
    TInstant Start;
};

TTraceRecorder& GetRecorder();

} // namespace TTimingTrace