#include <afina/coroutine/Engine.h>

#include <setjmp.h>
#include <stdio.h>
#include <cstring>
#include <cassert>

namespace Afina {
namespace Coroutine {

void Engine::set_grows_upwards() {
	if (!StackBottom) {
		char here;
		StackBottom = &here;
		set_grows_upwards();
		StackBottom = 0;
	} else {
		char here;
		grows_upwards = (&here > StackBottom);
	}
}

void Engine::Store(context &ctx) {
	char EndOfStack;
	if (grows_upwards) {
		ctx.High = &EndOfStack + 1;
	} else {
		ctx.Low = &EndOfStack;
	}
	assert(ctx.High > ctx.Low);
	auto stack_size = ctx.High - ctx.Low;
	if (std::get<1>(ctx.Stack) < stack_size || (std::get<1>(ctx.Stack) / 2 > stack_size)) {
		if (std::get<0>(ctx.Stack)) {
			delete[] std::get<0>(ctx.Stack);
		}
		std::get<0>(ctx.Stack) = new char[stack_size];
		std::get<1>(ctx.Stack) = stack_size;
	}
	std::memcpy(std::get<0>(ctx.Stack), ctx.Low, stack_size);
}

void Engine::Restore(context &ctx) {
	char here;
	if (grows_upwards) {
		while (&here <= ctx.High) {
			Restore(ctx);
		}
	} else {
		while (&here >= ctx.Low) {
			Restore(ctx);
		}
	}
	std::memcpy(ctx.Low, std::get<0>(ctx.Stack), ctx.High - ctx.Low);
	cur_routine = &ctx;
	longjmp(ctx.Environment, 1);
}

void Engine::yield() {
	if (cur_routine) {
		if (alive == cur_routine && !alive->next) {
			// cur_routine is the only alive coroutine
			return;
		}
		
		// Save context
		if (setjmp(cur_routine->Environment) > 0) {
			return;
		}
		Store(*cur_routine);
		
		assert(alive != nullptr);
		if (alive == cur_routine) {
			Restore(*alive->next);
		} else {
			Restore(*alive);
		}
	} else {
		// We are in idle_ctx
		if (alive) {
			Restore(*alive);
		} else {
			return;
		}
	}
}

void Engine::sched(void *routine) {
	auto coroutine = static_cast<context *>(routine);
	if (!coroutine) {
		yield();
	} else if (coroutine == cur_routine) {
		return;
	} else {
		if (setjmp(cur_routine->Environment) > 0) {
			return;
		}
		Store(*cur_routine);
		Restore(*coroutine);
	}
}

void Engine::block(void *coro) {
	auto coroutine = static_cast<context *>(coro);
	if (!coroutine) {
		assert (cur_routine != nullptr);
		coroutine = cur_routine;
	}
	if (coroutine->prev) {
		coroutine->prev->next = coroutine->next;
	} else {
		alive = coroutine->next;
		alive->prev = nullptr;
	}
	if (coroutine->next) {
		coroutine->next->prev = coroutine->prev;
	}
	coroutine->prev = nullptr;
	coroutine->next = blocked;
	blocked = coroutine;
	if (blocked->next) {
		blocked->next->prev = blocked;
	}
	if (coroutine == cur_routine) {
		if (setjmp(cur_routine->Environment) > 0) {
			return;
		} else {
			Store(*cur_routine);
			cur_routine = nullptr;
			Restore(*idle_ctx);
		}
	}
}

void Engine::unblock(void *coro) {
	auto coroutine = static_cast<context *>(coro);
	for (auto routine = blocked; routine != nullptr; routine = routine->next) {
		if (routine == coroutine) {
			if (routine->prev) {
				routine->prev->next = routine->next;
			} else {
				blocked = routine->next;
				blocked->prev = nullptr;
			}
			if (routine->next) {
				routine->next->prev = routine->prev;
			}
			routine->prev = nullptr;
			routine->next = alive;
			alive = routine;
			if (alive->next) {
				alive->next->prev = alive;
			}
			break;
		}
	}
}

} // namespace Coroutine
} // namespace Afina
