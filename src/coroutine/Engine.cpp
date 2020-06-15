#include <afina/coroutine/Engine.h>

#include <cassert>
#include <cstring>
#include <setjmp.h>
#include <stdio.h>

#include <iostream>

namespace Afina {
namespace Coroutine {

void Engine::Store(context &ctx) {
    assert(&ctx != idle_ctx);
    // idle_ctx should be stored in a special way

    char EndOfStack;
    if (&EndOfStack > StackBottom) {
        ctx.High = &EndOfStack + 1;
    } else {
        ctx.Low = &EndOfStack;
    }
    assert(ctx.Low > 0 && ctx.High > ctx.Low);
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
    assert(&ctx != idle_ctx);
    // idle_ctx should be restored in a special way

    assert(ctx.Low > 0 && ctx.High > ctx.Low);
    char here;
    while (&here <= ctx.High && &here >= ctx.Low) {
        Restore(ctx);
    }
    assert(std::get<1>(ctx.Stack) >= ctx.High - ctx.Low);
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
        return;
    } else if (coroutine == cur_routine) {
        return;
    } else {
        if (cur_routine) {
            if (setjmp(cur_routine->Environment) > 0) {
                return;
            }
            Store(*cur_routine);
        }
        Restore(*coroutine);
    }
}

void Engine::block(void *coro) {
    auto coroutine = static_cast<context *>(coro);
    if (!coroutine) {
        assert(cur_routine != nullptr);
        coroutine = cur_routine;
    }
    if (coroutine->prev) {
        coroutine->prev->next = coroutine->next;
    } else {
        alive = coroutine->next;
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
            longjmp(idle_ctx->Environment, 1);
        }
    }
}

void Engine::unblock(void *coro) {
    assert(coro != nullptr);
    auto routine = static_cast<context *>(coro);
    if (routine->prev) {
        routine->prev->next = routine->next;
    } else if (blocked == routine) {
        blocked = routine->next;
    } else {
        assert(alive == routine);
        return;
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
}

} // namespace Coroutine
} // namespace Afina
