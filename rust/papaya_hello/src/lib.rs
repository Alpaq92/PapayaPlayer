/// Returns 42; called from C to prove the Rust build and link work.
#[no_mangle]
pub extern "C" fn papaya_rust_hello() -> u32 {
    42
}
