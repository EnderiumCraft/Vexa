// The Rust halves of SpiderMonkey's C++ link on Vexa: the normalizer glue (String
// normalize), encoding_c (its UTF-8 and ASCII checks) and encoding_c_mem (conversions).
// Linking the crates into one static archive keeps their #[no_mangle] entry points.
extern crate encoding_c;
extern crate encoding_c_mem;
extern crate mozjs_normalizer_glue;
