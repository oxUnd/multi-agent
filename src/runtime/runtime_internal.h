#ifndef MORPH_RUNTIME_INTERNAL_H
#define MORPH_RUNTIME_INTERNAL_H

#include "runtime/context_owner.h"
#include "runtime/execute.h"
#include "runtime/runtime.h"
#include "runtime/task_worker.h"

struct runtime {
	struct runtime_context context;
	struct runtime_options options;
	struct scheduled_task_event_sink task_events;
	struct runtime_task_worker task_worker;
	struct morph_sync_worker sync_worker;
	int sync_started;
};

int runtime_lock_turn(struct runtime_engine *engine,
		      const struct runtime_request *request,
		      struct runtime_result *result);
void runtime_unlock_turn(struct runtime_engine *engine);

int runtime_prepare_turn(void *user_data,
			 const struct runtime_request *request);
void runtime_finish_turn(void *user_data,
			  const struct runtime_request *request,
			  const struct runtime_result *result);
void runtime_record_usage(const struct model_usage *usage, void *user_data);

int runtime_ensure_current_session(struct runtime *runtime);

#endif
