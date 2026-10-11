//! Evaluates a script on SpiderMonkey from Rust, with the same check as
//! tools/mozjs/vexa-js-test.cpp: the Rust side sees the same result the C++ side does.

use std::process::ExitCode;
use std::ptr;

use mozjs::conversions::{ConversionResult, FromJSValConvertible};
use mozjs::jsapi::OnNewGlobalHookOption;
use mozjs::jsval::UndefinedValue;
use mozjs::rooted;
use mozjs::rust::wrappers2::JS_NewGlobalObject;
use mozjs::rust::{
    evaluate_script, CompileOptionsWrapper, JSEngine, RealmOptions, Runtime, SIMPLE_GLOBAL_CLASS,
};

const SCRIPT: &str = "
var sum = 0;
for (var i = 1; i <= 10; i++) sum += i;
var objs = [];
for (var j = 0; j < 20000; j++) objs.push({n: j, s: 'x' + j});
objs = null;
var nfd = 'café'.normalize('NFD').length;
'sum=' + sum + ' nfd=' + nfd + ' upper=' + 'vexa'.toUpperCase() + ' json=' + JSON.stringify({a: [1, 2]});
";

const EXPECTED: &str = "sum=55 nfd=5 upper=VEXA json={\"a\":[1,2]}";

fn main() -> ExitCode {
    let engine = JSEngine::init().expect("failed to initialize the JS engine");
    let mut rt = Runtime::new(engine.handle());

    let options = RealmOptions::default();
    rooted!(&in(rt.cx()) let global = unsafe {
        JS_NewGlobalObject(rt.cx(), &SIMPLE_GLOBAL_CLASS, ptr::null_mut(),
                           OnNewGlobalHookOption::FireOnNewGlobalHook,
                           &*options)
    });
    rooted!(&in(rt.cx()) let mut rval = UndefinedValue());

    let compile = CompileOptionsWrapper::new(rt.cx_no_gc(), c"vexa-js-rust.js".to_owned(), 1);
    if evaluate_script(rt.cx(), global.handle(), SCRIPT, rval.handle_mut(), compile).is_err() {
        eprintln!("rust js check: evaluation failed");
        return ExitCode::from(2);
    }

    let got = match String::from_jsval(rt.cx(), rval.handle(), ()) {
        Ok(ConversionResult::Success(s)) => s,
        _ => {
            eprintln!("rust js check: result is not a string");
            return ExitCode::from(3);
        }
    };
    println!("rust js check: {got}");
    if got == EXPECTED {
        println!("rust js check: pass");
        ExitCode::SUCCESS
    } else {
        eprintln!("rust js check: FAIL, expected {EXPECTED:?}");
        ExitCode::from(4)
    }
}
