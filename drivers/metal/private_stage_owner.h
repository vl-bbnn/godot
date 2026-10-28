// Private standalone MetalCPP ownership prototype. Not yet a Godot runtime hook.
#pragma once
#include <Metal/Metal.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <functional>

namespace PrivateMetalStageOwner {
constexpr unsigned ROW_CAPACITY=8192;
constexpr unsigned ENCODERS_PER_BATCH=4;
enum class Status { Disabled, Ready, UnsupportedBoundary, UnsupportedScope, MissingTimestamp, MissingSink, Conflict, BatchFull, SinkFull, AllocationFailed, InvalidDescriptor };
enum class Error { None, Abandoned, GPUFailed, ResolveFailed, BadSample, AllocationFailed, EncoderFailed };
struct Metrics {
    std::atomic<unsigned> created{0}, destroyed{0}, sample_allocations{0}, sample_owner_releases{0}, callbacks{0}, completed{0}, clock_calls{0};
};
inline Metrics metrics;
struct Row {
    unsigned batch=0, encoder=0;
    std::array<uint64_t,4> samples{};
    uint64_t cpu_start_ns=0, cpu_end_ns=0, gpu_start_raw=0, gpu_end_raw=0;
    unsigned status=0;
    int64_t gpu_error=0;
    Error error=Error::None;
    bool completed=false, valid=false;
};
struct Sink {
    unsigned limit=ROW_CAPACITY;
    std::atomic<unsigned> attempts{0}, reserved{0}, batch_dropped{0}, sink_dropped{0}, conflicts{0}, callbacks_done{0};
    std::array<Row,ROW_CAPACITY> rows{};
    std::atomic<unsigned> seen_cb{0}, selected_cb{0}, cadence_skipped{0}, full_cb_skipped{0}, invalid_descriptor{0}, other_device_encoders{0};
    std::function<void(const Sink &)> finalize;
    ~Sink(){if(finalize)finalize(*this);}
};
class Batch : public std::enable_shared_from_this<Batch> {
    NS::SharedPtr<MTL::Device> device;
    NS::SharedPtr<MTL::CounterSet> counter_set;
    NS::SharedPtr<MTL::CounterSampleBuffer> sample_buffer;
    std::shared_ptr<Sink> sink;
    std::array<unsigned,ENCODERS_PER_BATCH> indices{};
    unsigned batch_id=0, count=0;
    bool registered=false;
    MTL::Timestamp cpu_start=0,gpu_start=0;

    Batch(MTL::Device *p_device,MTL::CounterSet *p_set,std::shared_ptr<Sink> p_sink,unsigned p_id):
        device(NS::RetainPtr(p_device)),counter_set(NS::RetainPtr(p_set)),sink(std::move(p_sink)),batch_id(p_id) {
        metrics.created.fetch_add(1,std::memory_order_relaxed);
    }
    void complete(MTL::CommandBuffer *commands) {
        auto pool=NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
        MTL::Timestamp cpu_end=0,gpu_end=0;
        device->sampleTimestamps(&cpu_end,&gpu_end);
        metrics.clock_calls.fetch_add(1,std::memory_order_relaxed);
        const unsigned status=unsigned(commands->status());
        const int64_t error=commands->error()?commands->error()->code():0;
        NS::Data *resolved=nullptr;
        if (status==unsigned(MTL::CommandBufferStatusCompleted) && !error) {
            resolved=sample_buffer->resolveCounterRange(NS::Range(0,count*4));
        }
        const bool size_ok=resolved && resolved->length()==count*4*sizeof(MTL::CounterResultTimestamp);
        for (unsigned i=0;i<count;++i) {
            Row &row=sink->rows[indices[i]];
            row.cpu_start_ns=cpu_start;row.cpu_end_ns=cpu_end;row.gpu_start_raw=gpu_start;row.gpu_end_raw=gpu_end;
            row.status=status;row.gpu_error=error;row.completed=true;
            if(row.error==Error::EncoderFailed) continue;
            if (status!=unsigned(MTL::CommandBufferStatusCompleted) || error) row.error=Error::GPUFailed;
            else if (!size_ok) row.error=Error::ResolveFailed;
            else {
                MTL::CounterResultTimestamp values[4];
                std::memcpy(values,static_cast<const uint8_t *>(resolved->bytes())+i*sizeof(values),sizeof(values));
                for (unsigned j=0;j<4;++j) row.samples[j]=values[j].timestamp;
                bool valid=cpu_end>cpu_start && gpu_end>gpu_start;
                for (uint64_t value:row.samples) valid=valid && value!=0 && value!=MTL::CounterErrorValue && value>=gpu_start && value<=gpu_end;
                valid=valid && row.samples[1]>=row.samples[0] && row.samples[3]>=row.samples[2];
                row.error=valid?Error::None:Error::BadSample;row.valid=valid;
            }
        }
        metrics.completed.fetch_add(1,std::memory_order_acq_rel);
        sink->callbacks_done.fetch_add(1,std::memory_order_acq_rel);
    }
public:
    ~Batch() {
        if (!registered) for (unsigned i=0;i<count;++i) if(sink->rows[indices[i]].error==Error::None)sink->rows[indices[i]].error=Error::Abandoned;
        if (sample_buffer) metrics.sample_owner_releases.fetch_add(1,std::memory_order_relaxed);
        metrics.destroyed.fetch_add(1,std::memory_order_relaxed);
    }
    static std::shared_ptr<Batch> create(bool enabled,MTL::Device *device,std::shared_ptr<Sink> sink,unsigned id,Status &status,MTL::CounterSamplingPoint boundary=MTL::CounterSamplingPointAtStageBoundary) {
        if (!enabled) {status=Status::Disabled;return {};}
        if (!device || !device->supportsCounterSampling(boundary)) {status=Status::UnsupportedBoundary;return {};}
        if (boundary!=MTL::CounterSamplingPointAtStageBoundary) {status=Status::UnsupportedScope;return {};}
        if (!sink || sink->limit>ROW_CAPACITY) {status=Status::MissingSink;return {};}
        MTL::CounterSet *timestamps=nullptr;
        NS::Array *sets=device->counterSets();
        for (NS::UInteger i=0;sets && i<sets->count();++i) {
            auto *set=sets->object<MTL::CounterSet>(i);
            if (set->name()->isEqualToString(MTL::CommonCounterSetTimestamp)) timestamps=set;
        }
        bool found=false;
        if (timestamps) {
            NS::Array *counters=timestamps->counters();
            for (NS::UInteger i=0;i<counters->count();++i) if (counters->object<MTL::Counter>(i)->name()->isEqualToString(MTL::CommonCounterTimestamp)) found=true;
        }
        if (!found) {status=Status::MissingTimestamp;return {};}
        status=Status::Ready;
        return std::shared_ptr<Batch>(new Batch(device,timestamps,std::move(sink),id));
    }
    NS::SharedPtr<MTL::RenderPassDescriptor> descriptor(MTL::RenderPassDescriptor *original,Status &status) {
        sink->attempts.fetch_add(1,std::memory_order_relaxed);
        if (original->sampleBufferAttachments()->object(0)->sampleBuffer()) {
            sink->conflicts.fetch_add(1,std::memory_order_relaxed);status=Status::Conflict;return {};
        }
        if (count==ENCODERS_PER_BATCH) {
            sink->batch_dropped.fetch_add(1,std::memory_order_relaxed);status=Status::BatchFull;return {};
        }
        const unsigned index=sink->reserved.fetch_add(1,std::memory_order_relaxed);
        if (index>=sink->limit) {
            sink->sink_dropped.fetch_add(1,std::memory_order_relaxed);status=Status::SinkFull;return {};
        }
        if (!sample_buffer) {
            auto descriptor=NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
            descriptor->setCounterSet(counter_set.get());descriptor->setStorageMode(MTL::StorageModeShared);descriptor->setSampleCount(ENCODERS_PER_BATCH*4);
            NS::Error *error=nullptr;
            sample_buffer=NS::TransferPtr(device->newCounterSampleBuffer(descriptor.get(),&error));
            if (!sample_buffer) {
                Row &row=sink->rows[index];row.error=Error::AllocationFailed;status=Status::AllocationFailed;return {};
            }
            metrics.sample_allocations.fetch_add(1,std::memory_order_relaxed);
        }
        auto copy=NS::TransferPtr(original->copy());
        auto *attachment=copy->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(sample_buffer.get());attachment->setStartOfVertexSampleIndex(count*4);attachment->setEndOfVertexSampleIndex(count*4+1);
        attachment->setStartOfFragmentSampleIndex(count*4+2);attachment->setEndOfFragmentSampleIndex(count*4+3);
        Row &row=sink->rows[index];row.batch=batch_id;row.encoder=count;indices[count]=index;++count;
        status=Status::Ready;
        return copy;
    }
    MTL::RenderCommandEncoder *new_encoder(MTL::CommandBuffer *commands,MTL::RenderPassDescriptor *original,Status &status) {
        if(!original){sink->invalid_descriptor.fetch_add(1,std::memory_order_relaxed);status=Status::InvalidDescriptor;return nullptr;}
        auto copy=descriptor(original,status);
        auto *encoder=commands->renderCommandEncoder(copy?copy.get():original);
        if(copy && !encoder)sink->rows[indices[count-1]].error=Error::EncoderFailed;
        return encoder;
    }
    void before_commit(MTL::CommandBuffer *commands) {
        if (!count) return;
        device->sampleTimestamps(&cpu_start,&gpu_start);metrics.clock_calls.fetch_add(1,std::memory_order_relaxed);
        registered=true;metrics.callbacks.fetch_add(1,std::memory_order_relaxed);
        // Captures sample-buffer/device/sink owner only. No CB or renderer owner.
        MTL::HandlerFunction handler=[owner=shared_from_this()](MTL::CommandBuffer *completed){owner->complete(completed);};
        commands->addCompletedHandler(handler);
    }
};
} // namespace PrivateMetalStageOwner
