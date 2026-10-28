// Private standalone MetalCPP ownership prototype. Not yet a Godot runtime hook.
#pragma once
#include <Metal/Metal.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <functional>
#include <cstdio>

namespace PrivateMetalStageOwner {
constexpr unsigned ROW_CAPACITY=32768;
constexpr unsigned ENCODERS_PER_BATCH=64;
enum class Status { Disabled, Ready, UnsupportedBoundary, UnsupportedScope, MissingTimestamp, MissingSink, Conflict, BatchFull, SinkFull, AllocationFailed, InvalidDescriptor };
enum class Error { None, Abandoned, GPUFailed, ResolveFailed, BadSample, AllocationFailed, EncoderFailed, StageUnavailable };
struct Metrics {
    std::atomic<unsigned> created{0}, destroyed{0}, sample_allocations{0}, sample_owner_releases{0}, callbacks{0}, completed{0}, clock_calls{0};
};
inline Metrics metrics;
struct Row {
    unsigned batch=0, encoder=0;
    unsigned draw_commands=0, indirect_commands=0, zero_count_commands=0;
    unsigned fragment_draws=0, no_fragment_draws=0, unknown_fragment_draws=0;
    unsigned color_mask=0, clear_mask=0;
    bool depth_attachment=false, stencil_attachment=false;
    int64_t allocation_error_code=0;
    std::array<uint64_t,4> samples{};
    uint64_t cpu_start_ns=0, cpu_end_ns=0, gpu_start_raw=0, gpu_end_raw=0;
    unsigned status=0;
    int64_t gpu_error=0;
    Error error=Error::None;
    bool completed=false, valid=false, capture_valid=false, vertex_available=false, fragment_available=false;
};
// Nonzero raw samples require current two-point reference bounds. Never zero-duration.
inline void classify(Row &row) {
    const bool clock=row.completed && row.status==unsigned(MTL::CommandBufferStatusCompleted) && !row.gpu_error && row.cpu_end_ns>row.cpu_start_ns && row.gpu_end_raw>row.gpu_start_raw;
    auto pair=[&](unsigned offset){auto a=row.samples[offset],b=row.samples[offset+1];return clock && a && b && a!=MTL::CounterErrorValue && b!=MTL::CounterErrorValue && a>=row.gpu_start_raw && b<=row.gpu_end_raw && b>=a;};
    row.vertex_available=pair(0);row.fragment_available=pair(2);
    row.valid=row.vertex_available && row.fragment_available;
    const bool unavailable=row.vertex_available && !row.samples[2] && !row.samples[3] && row.draw_commands==0;
    row.capture_valid=row.valid || unavailable;
    row.error=row.valid?Error::None:(unavailable?Error::StageUnavailable:Error::BadSample);
}
struct Sink {
    unsigned limit=ROW_CAPACITY;
    std::atomic<unsigned> attempts{0}, reserved{0}, batch_dropped{0}, sink_dropped{0}, conflicts{0}, callbacks_done{0};
    std::array<Row,ROW_CAPACITY> rows{};
    std::atomic<unsigned> seen_cb{0}, selected_cb{0}, cadence_skipped{0}, full_cb_skipped{0}, invalid_descriptor{0}, other_device_encoders{0};
    std::atomic<unsigned> max_encoder_attempts{0}, allocation_failures{0}, draw_metadata_misses{0};
    std::function<void(const Sink &)> finalize;
    ~Sink(){if(finalize)finalize(*this);}
};
class Batch : public std::enable_shared_from_this<Batch> {
    NS::SharedPtr<MTL::Device> device;
    NS::SharedPtr<MTL::CounterSet> counter_set;
    NS::SharedPtr<MTL::CounterSampleBuffer> sample_buffer;
    std::shared_ptr<Sink> sink;
    std::array<unsigned,ENCODERS_PER_BATCH> indices{};
    unsigned batch_id=0, count=0, attempt_count=0;
    MTL::RenderCommandEncoder *last_encoder=nullptr;
    Row *last_row=nullptr;
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
                classify(row);
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
        ++attempt_count;
        unsigned peak=sink->max_encoder_attempts.load(std::memory_order_relaxed);
        while(peak<attempt_count && !sink->max_encoder_attempts.compare_exchange_weak(peak,attempt_count,std::memory_order_relaxed)) {}
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
        Row &reserved_row=sink->rows[index];reserved_row.batch=batch_id;reserved_row.encoder=attempt_count-1;
        for(unsigned a=0;a<8;++a){auto *ca=original->colorAttachments()->object(a);if(ca->texture()){reserved_row.color_mask|=1u<<a;if(ca->loadAction()==MTL::LoadActionClear)reserved_row.clear_mask|=1u<<a;}}
        reserved_row.depth_attachment=original->depthAttachment()->texture()!=nullptr;reserved_row.stencil_attachment=original->stencilAttachment()->texture()!=nullptr;
        if (!sample_buffer) {
            auto descriptor=NS::TransferPtr(MTL::CounterSampleBufferDescriptor::alloc()->init());
            descriptor->setCounterSet(counter_set.get());descriptor->setStorageMode(MTL::StorageModeShared);descriptor->setSampleCount(ENCODERS_PER_BATCH*4);
            NS::Error *error=nullptr;
            sample_buffer=NS::TransferPtr(device->newCounterSampleBuffer(descriptor.get(),&error));
                    if (!sample_buffer) {
                Row &row=sink->rows[index];row.error=Error::AllocationFailed;row.allocation_error_code=error?error->code():0;
                sink->allocation_failures.fetch_add(1,std::memory_order_relaxed);
                std::fprintf(stderr,"PRIVATE METAL STAGE: counter allocation failed CB=%u encoder=%u code=%lld description=%s\n",batch_id,attempt_count-1,(long long)row.allocation_error_code,error?error->localizedDescription()->utf8String():"no NSError");
                status=Status::AllocationFailed;return {};
            }
            metrics.sample_allocations.fetch_add(1,std::memory_order_relaxed);
        }
        auto copy=NS::TransferPtr(original->copy());
        auto *attachment=copy->sampleBufferAttachments()->object(0);
        attachment->setSampleBuffer(sample_buffer.get());attachment->setStartOfVertexSampleIndex(count*4);attachment->setEndOfVertexSampleIndex(count*4+1);
        attachment->setStartOfFragmentSampleIndex(count*4+2);attachment->setEndOfFragmentSampleIndex(count*4+3);
        indices[count]=index;++count;
        status=Status::Ready;
        return copy;
    }
    MTL::RenderCommandEncoder *new_encoder(MTL::CommandBuffer *commands,MTL::RenderPassDescriptor *original,Status &status) {
        if(!original){sink->invalid_descriptor.fetch_add(1,std::memory_order_relaxed);status=Status::InvalidDescriptor;return nullptr;}
        last_row=nullptr;last_encoder=nullptr;
        auto copy=descriptor(original,status);
        auto *encoder=commands->renderCommandEncoder(copy?copy.get():original);
        if(copy) {last_encoder=encoder;last_row=&sink->rows[indices[count-1]];if(!encoder)last_row->error=Error::EncoderFailed;}
        return encoder;
    }
    // Passive diagnostics. Indirect element/instance counts remain unknown.
    void note_draw(MTL::RenderCommandEncoder *encoder,int fragment,uint64_t elements,uint64_t instances,bool indirect=false) {
        if(!last_row) return; // Explicitly unrecorded encoder (cap/conflict/allocation).
        if(last_encoder!=encoder){sink->draw_metadata_misses.fetch_add(1,std::memory_order_relaxed);return;}
        ++last_row->draw_commands;if(indirect)++last_row->indirect_commands;else if(!elements || !instances)++last_row->zero_count_commands;
        if(fragment>0)++last_row->fragment_draws;else if(fragment==0)++last_row->no_fragment_draws;else ++last_row->unknown_fragment_draws;
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
