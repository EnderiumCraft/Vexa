// Runs SpiderMonkey on Vexa: a runtime, a global object with the standard classes, a
// script that allocates enough to need a GC, and String.prototype.normalize (which goes
// through the Rust normalizer glue). Prints the result and checks it.
#include "jsapi.h"

#include "js/CharacterEncoding.h"
#include "js/CompilationAndEvaluation.h"
#include "js/CompileOptions.h"
#include "js/Conversions.h"
#include "js/Exception.h"
#include "js/GCAPI.h"
#include "js/GlobalObject.h"
#include "js/Initialization.h"
#include "js/RealmOptions.h"
#include "js/SourceText.h"

#include <cstdio>
#include <cstring>

// mozjs_sys installs its Rust hooks here. Only the optional OOM hook does anything,
// and this build does not enable it.
extern "C" void install_rust_hooks(void) {}

static const JSClass kGlobalClass = {"VexaGlobal", JSCLASS_GLOBAL_FLAGS,
                                     &JS::DefaultGlobalClassOps};

static const char16_t kScript[] = uR"JS(
var sum = 0;
for (var i = 1; i <= 10; i++) sum += i;
var objs = [];
for (var j = 0; j < 20000; j++) objs.push({n: j, s: 'x' + j});
objs = null;
var nfd = 'café'.normalize('NFD').length;
'sum=' + sum + ' nfd=' + nfd + ' upper=' + 'vexa'.toUpperCase() + ' json=' + JSON.stringify({a: [1, 2]});
)JS";

static const char kExpected[] = "sum=55 nfd=5 upper=VEXA json={\"a\":[1,2]}";

int main() {
  if (!JS_Init()) {
    std::fprintf(stderr, "JS_Init failed\n");
    return 1;
  }
  JSContext* cx = JS_NewContext(8L * 1024 * 1024);
  if (!cx) {
    std::fprintf(stderr, "JS_NewContext failed\n");
    return 1;
  }
  // The self-hosted JavaScript builtins must exist before any global is created.
  if (!JS::InitSelfHostedCode(cx)) {
    std::fprintf(stderr, "JS::InitSelfHostedCode failed\n");
    JS_DestroyContext(cx);
    JS_ShutDown();
    return 1;
  }

  int status = 1;
  {
    JS::RealmOptions options;
    JS::RootedObject global(
        cx, JS_NewGlobalObject(cx, &kGlobalClass, nullptr,
                               JS::FireOnNewGlobalHook, options));
    if (!global) {
      std::fprintf(stderr, "JS_NewGlobalObject failed\n");
      JS_DestroyContext(cx);
      JS_ShutDown();
      return 1;
    }
    JSAutoRealm ar(cx, global);

    JS::CompileOptions compile(cx);
    compile.setFileAndLine("vexa-js-test.js", 1);
    JS::SourceText<char16_t> source;
    JS::RootedValue rval(cx);
    bool ok = source.init(cx, kScript, std::char_traits<char16_t>::length(kScript),
                          JS::SourceOwnership::Borrowed) &&
              JS::Evaluate(cx, compile, source, &rval);
    if (!ok) {
      JS::RootedValue exception(cx);
      if (JS_GetPendingException(cx, &exception)) {
        JS::RootedString text(cx, JS::ToString(cx, exception));
        JS::UniqueChars chars = JS_EncodeStringToUTF8(cx, text);
        std::fprintf(stderr, "evaluation threw: %s\n", chars ? chars.get() : "?");
      } else {
        std::fprintf(stderr, "evaluation failed\n");
      }
    } else {
      JS::RootedString result(cx, JS::ToString(cx, rval));
      JS::UniqueChars chars = JS_EncodeStringToUTF8(cx, result);
      if (!chars) {
        std::fprintf(stderr, "could not encode the result\n");
      } else {
        std::printf("result: %s\n", chars.get());
        JS_GC(cx);
        std::printf("after a GC: still running\n");
        status = std::strcmp(chars.get(), kExpected) == 0 ? 0 : 2;
        std::printf(status == 0 ? "js runtime check: pass\n"
                                : "js runtime check: FAIL, expected: %s\n",
                    kExpected);
      }
    }
  }
  JS_DestroyContext(cx);
  JS_ShutDown();
  return status;
}
