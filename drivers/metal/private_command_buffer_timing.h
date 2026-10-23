// Private opt-in diagnostic. No Godot singleton, rendering resources or owner
// pointers survive in the callback. This is command-buffer time, not frame time.
#pragma once

#include <Metal/Metal.hpp>
#include <mach/mach_time.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace PrivateMetalTiming {
constexpr uint64_t ROW_LIMIT = 16384;

struct Row {
	uint64_t registered_tick = 0;
	uint64_t completed_tick = 0;
	double gpu_start_seconds = 0;
	double gpu_end_seconds = 0;
	int64_t error_code = 0;
	uint32_t status = 0;
	bool completed = false;
};

struct State {
	FILE *file = nullptr;
	mach_timebase_info_data_t timebase = {};
	std::atomic<uint64_t> submitted{ 0 };
	std::atomic<uint64_t> skipped_null_calls{ 0 };
	std::array<Row, ROW_LIMIT> rows;

	~State() {
		// Every callback owns a shared reference. Destruction happens after the
		// final callback; no per-buffer disk IO or extra game/GPU synchronization.
		if (!file) {
			return;
		}
		const uint64_t count = submitted.load(std::memory_order_relaxed);
		const uint64_t recorded = count < ROW_LIMIT ? count : ROW_LIMIT;
		bool failed = false;
		for (uint64_t i = 0; i < recorded; ++i) {
			const Row &r = rows[i];
			const bool valid = r.completed && r.status == uint32_t(MTL::CommandBufferStatusCompleted) &&
					std::isfinite(r.gpu_start_seconds) && std::isfinite(r.gpu_end_seconds) &&
					r.gpu_start_seconds > 0 && r.gpu_end_seconds >= r.gpu_start_seconds;
			failed |= fprintf(file, "%llu,%llu,%llu,%.9f,%.9f,%u,%lld,%u,%u\n",
					(unsigned long long)i, (unsigned long long)r.registered_tick,
					(unsigned long long)r.completed_tick, r.gpu_start_seconds, r.gpu_end_seconds,
					r.status, (long long)r.error_code, unsigned(r.completed), unsigned(valid)) < 0;
		}
		failed |= fprintf(file, "# submitted=%llu recorded=%llu truncated=%llu skipped_null_calls=%llu\n",
				(unsigned long long)count, (unsigned long long)recorded,
				(unsigned long long)(count - recorded),
				(unsigned long long)skipped_null_calls.load(std::memory_order_relaxed)) < 0;
		failed |= ferror(file) != 0;
		failed |= fclose(file) != 0;
		if (failed) {
			fprintf(stderr, "PRIVATE METAL TIMING: output write failed\n");
		}
	}
};

inline std::shared_ptr<State> create_state() {
	const char *path = getenv("OPENBFME_METAL_COMMAND_BUFFER_TIMING");
	if (!path || !*path) {
		return {};
	}
	auto state = std::make_shared<State>();
	if (mach_timebase_info(&state->timebase) != KERN_SUCCESS) {
		fprintf(stderr, "PRIVATE METAL TIMING: mach timebase unavailable\n");
		return {};
	}
	// Never replace existing output or follow an existing output symlink.
	state->file = fopen(path, "wx");
	if (!state->file) {
		fprintf(stderr, "PRIVATE METAL TIMING: cannot create new output\n");
		return {};
	}
	fprintf(state->file, "# private-metal-command-buffer-timing-v1\n"
			"# scope=MDCommandBuffer::commit only; raw helper copy/wait/signal buffers excluded\n"
			"# not viewport/pass/frame GPU time; overlapping buffers must not be blindly summed\n"
			"# completed records buffered until clean shutdown; abnormal exits may lose samples\n"
			"# mach_timebase_numer=%u mach_timebase_denom=%u row_limit=%llu\n"
			"submission,registered_tick,completed_tick,gpu_start_seconds,gpu_end_seconds,status,error_code,completed,valid\n",
			state->timebase.numer, state->timebase.denom, (unsigned long long)ROW_LIMIT);
	fflush(state->file);
	return state;
}

inline void before_commit(MTL::CommandBuffer *buffer) {
	static const std::shared_ptr<State> state = create_state();
	if (!state) {
		return;
	}
	if (!buffer) {
		state->skipped_null_calls.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const uint64_t index = state->submitted.fetch_add(1, std::memory_order_relaxed);
	if (index >= ROW_LIMIT) {
		return;
	}
	state->rows[index].registered_tick = mach_absolute_time();
	buffer->addCompletedHandler([owner = state, index](MTL::CommandBuffer *completed) {
		Row &row = owner->rows[index];
		row.completed_tick = mach_absolute_time();
		row.gpu_start_seconds = completed->GPUStartTime();
		row.gpu_end_seconds = completed->GPUEndTime();
		row.status = uint32_t(completed->status());
		if (auto error = completed->error()) {
			row.error_code = error->code();
		}
		row.completed = true;
	});
}
} // namespace PrivateMetalTiming
