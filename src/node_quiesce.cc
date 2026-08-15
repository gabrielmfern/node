#include "env-inl.h"
#include "handle_wrap.h"
#include "node_external_reference.h"
#include "node_internals.h"
#include "req_wrap-inl.h"
#include "threadpoolwork-inl.h"
#include "util-inl.h"
#include "v8.h"

#include <string>

namespace node {
namespace quiesce {

using v8::Array;
using v8::BigInt;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::Local;
using v8::LocalVector;
using v8::Object;
using v8::Value;

static void Checkpoint(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  args.GetReturnValue().Set(
      BigInt::NewFromUnsigned(env->isolate(), env->BumpQuiesceGeneration()));
}

struct ForeignWalk {
  Environment* env;
  LocalVector<Value>* resources_info;
};

static void CountForeign(uv_handle_t* handle, void* arg) {
  auto* walk = static_cast<ForeignWalk*>(arg);
  if (handle->data == nullptr && !uv_is_closing(handle)) {
    std::string name = std::string("foreign:") + uv_handle_type_name(handle->type);

    Local<Object> entry = Object::New(walk->env->isolate());
    entry->Set(walk->env->context(), FIXED_ONE_BYTE_STRING(walk->env->isolate(), "type"),
               OneByteString(walk->env->isolate(), name.c_str())).Check();
    entry->Set(walk->env->context(), FIXED_ONE_BYTE_STRING(walk->env->isolate(), "asyncId"),
               v8::Number::New(walk->env->isolate(), -1)).Check();
    walk->resources_info->emplace_back(entry);
  }
}

static void ReportNative(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsBigInt());
  uint64_t checkpoint = args[0].As<BigInt>()->Uint64Value();

  LocalVector<Value> resources_info(env->isolate());

  // Active requests
  for (ReqWrapBase* req_wrap : *env->req_wrap_queue()) {
    AsyncWrap* w = req_wrap->GetAsyncWrap();
    if (!w->persistent().IsEmpty() &&
        req_wrap->quiesce_generation() >= checkpoint) {
      Local<Object> entry = Object::New(env->isolate());
      entry->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "type"),
                 OneByteString(env->isolate(), w->MemoryInfoName())).Check();
      entry->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "asyncId"),
                 v8::Number::New(env->isolate(), w->get_async_id())).Check();
      resources_info.emplace_back(entry);
    }
  }

  // Active handles
  for (HandleWrap* w : *env->handle_wrap_queue()) {
    if (!w->persistent().IsEmpty() && w->quiesce_generation() >= checkpoint) {
      Local<Object> entry = Object::New(env->isolate());
      entry->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "type"),
                 OneByteString(env->isolate(), w->MemoryInfoName())).Check();
      entry->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "asyncId"),
                 v8::Number::New(env->isolate(), w->get_async_id())).Check();
      resources_info.emplace_back(entry);
    }
  }

  // These are created from napi addons
  for (ThreadPoolWork* w : *env->pool_works()) {
    if (w->quiesce_generation() >= checkpoint) {
      Local<Object> entry = Object::New(env->isolate());
      entry->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "type"),
                 OneByteString(env->isolate(), "ThreadPoolWork")).Check();
      entry->Set(env->context(), FIXED_ONE_BYTE_STRING(env->isolate(), "asyncId"),
                 v8::Number::New(env->isolate(), -1)).Check();
      resources_info.emplace_back(entry);
    }
  }

  // Active timeouts, intervals and immediates
  Local<Value> cp_arg = args[0];
  Local<Value> counts;
  if (!env->quiesce_count_timers_function()
           ->Call(env->context(), v8::Undefined(env->isolate()), 1, &cp_arg)
           .ToLocal(&counts)) {
    return;
  }

  ForeignWalk walk{env, &resources_info};
  uv_walk(env->event_loop(), CountForeign, &walk);

  Local<Object> result = Object::New(env->isolate());
  result
      ->Set(env->context(),
            FIXED_ONE_BYTE_STRING(env->isolate(), "resources"),
            Array::New(
                env->isolate(), resources_info.data(), resources_info.size()))
      .Check();
  result
      ->Set(env->context(),
            FIXED_ONE_BYTE_STRING(env->isolate(), "timers"),
            counts)
      .Check();

  args.GetReturnValue().Set(result);
}

static void QuiesceNative(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsBigInt());
  uint64_t checkpoint = args[0].As<BigInt>()->Uint64Value();

  Local<Value> cp_arg = args[0];
  USE(env->quiesce_wipe_timers_function()->Call(
      env->context(), v8::Undefined(env->isolate()), 1, &cp_arg));

  // Active requests
  for (ReqWrapBase* req_wrap : *env->req_wrap_queue()) {
    AsyncWrap* w = req_wrap->GetAsyncWrap();
    if (!w->persistent().IsEmpty() &&
        req_wrap->quiesce_generation() >= checkpoint) {
      req_wrap->Cancel();
    }
  }

  // Active handles
  for (HandleWrap* w : *env->handle_wrap_queue()) {
    if (!w->persistent().IsEmpty() && HandleWrap::IsAlive(w) &&
        w->quiesce_generation() >= checkpoint) {
      w->Close();
    }
  }

  // These are created from napi addons
  for (ThreadPoolWork* w : *env->pool_works()) {
    if (w->quiesce_generation() >= checkpoint) {
      w->CancelWork();
    }
  }

  uv_walk(
      env->event_loop(),
      [](uv_handle_t* handle, void*) {
        if (handle->data != nullptr || uv_is_closing(handle)) return;
        uv_unref(handle);
      },
      nullptr);
}

static void SetTimerFunctions(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsFunction());
  CHECK(args[1]->IsFunction());
  Environment* env = Environment::GetCurrent(args);
  env->set_quiesce_count_timers_function(args[0].As<v8::Function>());
  env->set_quiesce_wipe_timers_function(args[1].As<v8::Function>());
}

static void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  SetMethod(context, target, "checkpoint", Checkpoint);
  SetMethod(context, target, "report", ReportNative);
  SetMethod(context, target, "quiesce", QuiesceNative);
  SetMethod(context, target, "setTimerFunctions", SetTimerFunctions);
}

static void RegisterExternalReferences(ExternalReferenceRegistry* registry) {
  registry->Register(Checkpoint);
  registry->Register(ReportNative);
  registry->Register(QuiesceNative);
  registry->Register(SetTimerFunctions);
}

}  // namespace quiesce
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(quiesce, node::quiesce::Initialize)
NODE_BINDING_EXTERNAL_REFERENCE(quiesce,
                                node::quiesce::RegisterExternalReferences)
