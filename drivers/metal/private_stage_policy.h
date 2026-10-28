#pragma once
#include "private_stage_owner.h"
#include <cerrno>
#include <cstdio>
#include <string>

namespace PrivateMetalStagePolicy {
using PrivateMetalStageOwner::Batch;
using PrivateMetalStageOwner::Sink;
enum class Status { Disabled, Ready, InvalidStride, UnsupportedBoundary, MissingTimestamp, OutputError, CadenceSkipped, SinkFull, BatchFailure, InvalidLimit };
struct Completion {std::atomic<bool> done{false},failed{false};};
class Trace {
    NS::SharedPtr<MTL::Device> device;
    std::shared_ptr<Sink> output;
    std::shared_ptr<Completion> completion;
    unsigned stride=30;
    Trace(MTL::Device *p_device,std::shared_ptr<Sink> p_output,unsigned p_stride,std::shared_ptr<Completion> p_completion):device(NS::RetainPtr(p_device)),output(std::move(p_output)),completion(std::move(p_completion)),stride(p_stride){}
public:
    static std::shared_ptr<Trace> create(const char *path,const char *stride_text,MTL::Device *device,Status &status,unsigned row_limit=PrivateMetalStageOwner::ROW_CAPACITY,MTL::CounterSamplingPoint point=MTL::CounterSamplingPointAtStageBoundary) {
        if(!path || !*path){status=Status::Disabled;return {};}
        if(!row_limit || row_limit>PrivateMetalStageOwner::ROW_CAPACITY){status=Status::InvalidLimit;return {};}
        unsigned stride=30;
        if(stride_text) {
            if(!*stride_text){status=Status::InvalidStride;return {};}
            uint64_t value=0;
            for(const char *p=stride_text;*p;++p){if(*p<'0'||*p>'9'){status=Status::InvalidStride;return {};}value=value*10+unsigned(*p-'0');if(value>4096){status=Status::InvalidStride;return {};}}
            if(!value){status=Status::InvalidStride;return {};}stride=unsigned(value);
        }
        if(!device || !device->supportsCounterSampling(point) || point!=MTL::CounterSamplingPointAtStageBoundary){status=Status::UnsupportedBoundary;return {};}
        bool timestamps=false;
        NS::Array *sets=device->counterSets();
        for(NS::UInteger i=0;sets && i<sets->count();++i){auto *set=sets->object<MTL::CounterSet>(i);if(set->name()->isEqualToString(MTL::CommonCounterSetTimestamp)){auto *counters=set->counters();for(NS::UInteger j=0;j<counters->count();++j)if(counters->object<MTL::Counter>(j)->name()->isEqualToString(MTL::CommonCounterTimestamp))timestamps=true;}}
        if(!timestamps){status=Status::MissingTimestamp;return {};}
        FILE *file=std::fopen(path,"wx");
        if(!file){status=Status::OutputError;return {};}
        if(std::fprintf(file,"# private-stage-policy-v1\n# scope=actual-render-encoder samples; CBsequence counts render-containing buffers, NOT frameID\n# rawGPUtimestamps plus CPU-reference ns; no per-frame/FPS claim\n# stride=%u row_limit=%u encoders_per_CB=%u\nindex,cb,encoder,vstart,vend,fstart,fend,cpu_start_ns,cpu_end_ns,gpu_start_raw,gpu_end_raw,status,gpu_error,error,completed,valid,draw_commands,indirect_commands,zero_count_commands,fragment_draws,no_fragment_draws,unknown_fragment_draws,color_mask,clear_mask,depth_attachment,stencil_attachment,allocation_error_code\n",stride,row_limit,PrivateMetalStageOwner::ENCODERS_PER_BATCH)<0 || std::fflush(file)!=0){std::fclose(file);status=Status::OutputError;return {};}
        auto sink=std::make_shared<Sink>();sink->limit=row_limit;
        auto completion=std::make_shared<Completion>();
        // No Sink capture: finalizer runs exactly once after callback owners drain.
        sink->finalize=[file,completion](const Sink &s){
            unsigned attempted=s.reserved.load(std::memory_order_relaxed);unsigned count=attempted<s.limit?attempted:s.limit;bool failed=false;
            for(unsigned i=0;i<count;++i){const auto &r=s.rows[i];failed|=std::fprintf(file,"%u,%u,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%lld,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%lld\n",i,r.batch,r.encoder,(unsigned long long)r.samples[0],(unsigned long long)r.samples[1],(unsigned long long)r.samples[2],(unsigned long long)r.samples[3],(unsigned long long)r.cpu_start_ns,(unsigned long long)r.cpu_end_ns,(unsigned long long)r.gpu_start_raw,(unsigned long long)r.gpu_end_raw,r.status,(long long)r.gpu_error,unsigned(r.error),unsigned(r.completed),unsigned(r.valid),r.draw_commands,r.indirect_commands,r.zero_count_commands,r.fragment_draws,r.no_fragment_draws,r.unknown_fragment_draws,r.color_mask,r.clear_mask,unsigned(r.depth_attachment),unsigned(r.stencil_attachment),(long long)r.allocation_error_code)<0;}
            failed|=std::fprintf(file,"# seen_cb=%u selected_cb=%u cadence_skipped=%u full_cb_skipped=%u encoder_attempts=%u recorded=%u batch_dropped=%u sink_dropped=%u conflicts=%u invalid_descriptor=%u other_device_encoders=%u max_encoder_attempts=%u allocation_failures=%u draw_metadata_misses=%u\n",s.seen_cb.load(),s.selected_cb.load(),s.cadence_skipped.load(),s.full_cb_skipped.load(),s.attempts.load(),count,s.batch_dropped.load(),s.sink_dropped.load(),s.conflicts.load(),s.invalid_descriptor.load(),s.other_device_encoders.load(),s.max_encoder_attempts.load(),s.allocation_failures.load(),s.draw_metadata_misses.load())<0;
            failed|=std::ferror(file)!=0;failed|=std::fclose(file)!=0;
            if(failed)std::fprintf(stderr,"STAGE POLICY ERROR: output write failed\n");
            completion->failed.store(failed,std::memory_order_relaxed);completion->done.store(true,std::memory_order_release);
        };
        status=Status::Ready;
        return std::shared_ptr<Trace>(new Trace(device,std::move(sink),stride,std::move(completion)));
    }
    bool accepts_device(MTL::Device *other)const{return device.get()==other;}
    void skip_other_device(){output->other_device_encoders.fetch_add(1,std::memory_order_relaxed);}
    std::shared_ptr<Sink> sink() const{return output;}
    std::shared_ptr<Completion> completion_state() const{return completion;}
    std::shared_ptr<Batch> batch(Status &status) {
        unsigned sequence=output->seen_cb.fetch_add(1,std::memory_order_relaxed);
        if(sequence%stride){output->cadence_skipped.fetch_add(1,std::memory_order_relaxed);status=Status::CadenceSkipped;return {};}
        if(output->reserved.load(std::memory_order_relaxed)>=output->limit){output->full_cb_skipped.fetch_add(1,std::memory_order_relaxed);status=Status::SinkFull;return {};}
        PrivateMetalStageOwner::Status batch_status;
        auto result=Batch::create(true,device.get(),output,sequence,batch_status);
        if(!result){status=Status::BatchFailure;return {};}
        output->selected_cb.fetch_add(1,std::memory_order_relaxed);status=Status::Ready;return result;
    }
};
} // namespace PrivateMetalStagePolicy
