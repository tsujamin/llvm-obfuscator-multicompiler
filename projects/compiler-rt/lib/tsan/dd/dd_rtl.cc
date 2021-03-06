//===-- dd_rtl.cc ---------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "dd_rtl.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_placement_new.h"
#include "sanitizer_common/sanitizer_flags.h"
#include "sanitizer_common/sanitizer_stacktrace.h"
#include "sanitizer_common/sanitizer_stackdepot.h"

namespace __dsan {

static Context *ctx;

static u32 CurrentStackTrace(Thread *thr, uptr skip) {
  StackTrace trace;
  thr->ignore_interceptors = true;
  trace.Unwind(1000, 0, 0, 0, 0, 0, false);
  thr->ignore_interceptors = false;
  if (trace.size <= skip)
    return 0;
  return StackDepotPut(trace.trace + skip, trace.size - skip);
}

static void PrintStackTrace(Thread *thr, u32 stk) {
  uptr size = 0;
  const uptr *trace = StackDepotGet(stk, &size);
  thr->ignore_interceptors = true;
  StackTrace::PrintStack(trace, size);
  thr->ignore_interceptors = false;
}

static void ReportDeadlock(Thread *thr, DDReport *rep) {
  if (rep == 0)
    return;
  BlockingMutexLock lock(&ctx->report_mutex);
  Printf("==============================\n");
  Printf("WARNING: lock-order-inversion (potential deadlock)\n");
  for (int i = 0; i < rep->n; i++) {
    Printf("Thread %d locks mutex %llu while holding mutex %llu:\n",
      rep->loop[i].thr_ctx, rep->loop[i].mtx_ctx1, rep->loop[i].mtx_ctx0);
    PrintStackTrace(thr, rep->loop[i].stk[1]);
    if (rep->loop[i].stk[0]) {
      Printf("Mutex %llu was acquired here:\n",
        rep->loop[i].mtx_ctx0);
      PrintStackTrace(thr, rep->loop[i].stk[0]);
    }
  }
  Printf("==============================\n");
}

Callback::Callback(Thread *thr)
    : thr(thr) {
  lt = thr->dd_lt;
  pt = thr->dd_pt;
}

u32 Callback::Unwind() {
  return CurrentStackTrace(thr, 3);
}

void InitializeFlags(Flags *f, const char *env) {
  internal_memset(f, 0, sizeof(*f));

  // Default values.
  f->second_deadlock_stack = false;

  SetCommonFlagsDefaults(f);
  // Override some common flags defaults.
  f->allow_addr2line = true;

  // Override from command line.
  ParseFlag(env, &f->second_deadlock_stack, "second_deadlock_stack", "");
  ParseCommonFlagsFromString(f, env);

  // Copy back to common flags.
  *common_flags() = *f;
}

void Initialize() {
  static u64 ctx_mem[sizeof(Context) / sizeof(u64) + 1];
  ctx = new(ctx_mem) Context();

  InitializeInterceptors();
  InitializeFlags(flags(), GetEnv("DSAN_OPTIONS"));
  common_flags()->symbolize = true;
  ctx->dd = DDetector::Create(flags());
}

void ThreadInit(Thread *thr) {
  static atomic_uintptr_t id_gen;
  uptr id = atomic_fetch_add(&id_gen, 1, memory_order_relaxed);
  thr->dd_pt = ctx->dd->CreatePhysicalThread();
  thr->dd_lt = ctx->dd->CreateLogicalThread(id);
}

void ThreadDestroy(Thread *thr) {
  ctx->dd->DestroyPhysicalThread(thr->dd_pt);
  ctx->dd->DestroyLogicalThread(thr->dd_lt);
}

void MutexBeforeLock(Thread *thr, uptr m, bool writelock) {
  if (thr->ignore_interceptors)
    return;
  Callback cb(thr);
  {
    MutexHashMap::Handle h(&ctx->mutex_map, m);
    if (h.created())
      ctx->dd->MutexInit(&cb, &h->dd);
    ctx->dd->MutexBeforeLock(&cb, &h->dd, writelock);
  }
  ReportDeadlock(thr, ctx->dd->GetReport(&cb));
}

void MutexAfterLock(Thread *thr, uptr m, bool writelock, bool trylock) {
  if (thr->ignore_interceptors)
    return;
  Callback cb(thr);
  {
    MutexHashMap::Handle h(&ctx->mutex_map, m);
    if (h.created())
      ctx->dd->MutexInit(&cb, &h->dd);
    ctx->dd->MutexAfterLock(&cb, &h->dd, writelock, trylock);
  }
  ReportDeadlock(thr, ctx->dd->GetReport(&cb));
}

void MutexBeforeUnlock(Thread *thr, uptr m, bool writelock) {
  if (thr->ignore_interceptors)
    return;
  Callback cb(thr);
  {
    MutexHashMap::Handle h(&ctx->mutex_map, m);
    ctx->dd->MutexBeforeUnlock(&cb, &h->dd, writelock);
  }
  ReportDeadlock(thr, ctx->dd->GetReport(&cb));
}

void MutexDestroy(Thread *thr, uptr m) {
  if (thr->ignore_interceptors)
    return;
  Callback cb(thr);
  MutexHashMap::Handle h(&ctx->mutex_map, m, true);
  if (!h.exists())
    return;
  ctx->dd->MutexDestroy(&cb, &h->dd);
}

}  // namespace __dsan
