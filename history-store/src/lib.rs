//! Encrypted chat-history store with a small C ABI for the Qt GUI.
//!
//! All data crosses the boundary as UTF-8 JSON strings (see `store::Record`).
//! The handle is single-threaded from the caller's point of view: use it from
//! one thread at a time.

pub mod json;
pub mod rng;
pub mod secret;
pub mod store;

use std::ffi::{c_char, c_int, CStr, CString};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

use store::{KdfParams, Record, Store, StoreError};

pub const SC_OK: c_int = 0;
pub const SC_ERR_WRONG_PASSPHRASE: c_int = 1;
pub const SC_ERR_CORRUPT: c_int = 2;
pub const SC_ERR_EXISTS: c_int = 3;
pub const SC_ERR_IO: c_int = 4;
pub const SC_ERR_CRYPTO: c_int = 5;
pub const SC_ERR_ARG: c_int = 6;
pub const SC_ERR_PANIC: c_int = 7;

/// Opaque handle exposed to C.
pub struct ScStore(Store);

fn code(e: &StoreError) -> c_int {
    match e {
        StoreError::WrongPassphrase => SC_ERR_WRONG_PASSPHRASE,
        StoreError::Corrupt(_) => SC_ERR_CORRUPT,
        StoreError::AlreadyExists => SC_ERR_EXISTS,
        StoreError::Io(_) => SC_ERR_IO,
        StoreError::Crypto(_) => SC_ERR_CRYPTO,
    }
}

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        None
    } else {
        CStr::from_ptr(p).to_str().ok()
    }
}

unsafe fn set(err: *mut c_int, v: c_int) {
    if !err.is_null() {
        *err = v;
    }
}

/// Run `f`, turning a panic into `on_panic` (unwinding across `extern "C"` is UB).
fn guard<T>(on_panic: T, f: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(on_panic)
}

/// 1 if a history file exists at `path`, else 0.
#[no_mangle]
pub unsafe extern "C" fn sc_store_exists(path: *const c_char) -> c_int {
    match cstr(path) {
        Some(p) => Store::exists(Path::new(p)) as c_int,
        None => 0,
    }
}

/// Create a new, empty, encrypted history file. Returns NULL on error (`*err` set).
#[no_mangle]
pub unsafe extern "C" fn sc_store_create(path: *const c_char, pass: *const c_char, err: *mut c_int) -> *mut ScStore {
    guard(std::ptr::null_mut(), || {
        let (Some(path), Some(pass)) = (cstr(path), cstr(pass)) else {
            set(err, SC_ERR_ARG);
            return std::ptr::null_mut();
        };
        match Store::create(Path::new(path), pass, KdfParams::default()) {
            Ok(s) => {
                set(err, SC_OK);
                Box::into_raw(Box::new(ScStore(s)))
            }
            Err(e) => {
                set(err, code(&e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Open and decrypt. On success `*records_json` receives a JSON array of all
/// records (free with `sc_string_free`). Returns NULL on error (`*err` set).
#[no_mangle]
pub unsafe extern "C" fn sc_store_open(
    path: *const c_char,
    pass: *const c_char,
    records_json: *mut *mut c_char,
    err: *mut c_int,
) -> *mut ScStore {
    guard(std::ptr::null_mut(), || {
        let (Some(path), Some(pass)) = (cstr(path), cstr(pass)) else {
            set(err, SC_ERR_ARG);
            return std::ptr::null_mut();
        };
        match Store::open(Path::new(path), pass) {
            Ok((s, recs)) => {
                if !records_json.is_null() {
                    let json = Record::vec_to_json(&recs);
                    *records_json = CString::new(json).map(CString::into_raw).unwrap_or(std::ptr::null_mut());
                }
                set(err, SC_OK);
                Box::into_raw(Box::new(ScStore(s)))
            }
            Err(e) => {
                set(err, code(&e));
                std::ptr::null_mut()
            }
        }
    })
}

/// Encrypt and append one record (a JSON object). Returns 0 or an error code.
#[no_mangle]
pub unsafe extern "C" fn sc_store_append(s: *mut ScStore, record_json: *const c_char) -> c_int {
    guard(SC_ERR_PANIC, || {
        let (Some(s), Some(j)) = (s.as_mut(), cstr(record_json)) else { return SC_ERR_ARG };
        let Ok(rec) = Record::from_json(j) else { return SC_ERR_ARG };
        match s.0.append(&rec) {
            Ok(()) => SC_OK,
            Err(e) => code(&e),
        }
    })
}

/// Encrypt and append a JSON array of records with a single fsync. Returns 0 or an error code.
#[no_mangle]
pub unsafe extern "C" fn sc_store_append_batch(s: *mut ScStore, records_json: *const c_char) -> c_int {
    guard(SC_ERR_PANIC, || {
        let (Some(s), Some(j)) = (s.as_mut(), cstr(records_json)) else { return SC_ERR_ARG };
        let Ok(recs) = Record::vec_from_json(j) else { return SC_ERR_ARG };
        match s.0.append_batch(&recs) {
            Ok(()) => SC_OK,
            Err(e) => code(&e),
        }
    })
}

/// Replace the whole history by a JSON array of records (compaction/delete).
#[no_mangle]
pub unsafe extern "C" fn sc_store_rewrite(s: *mut ScStore, records_json: *const c_char) -> c_int {
    guard(SC_ERR_PANIC, || {
        let (Some(s), Some(j)) = (s.as_mut(), cstr(records_json)) else { return SC_ERR_ARG };
        let Ok(recs) = Record::vec_from_json(j) else { return SC_ERR_ARG };
        match s.0.rewrite(&recs) {
            Ok(()) => SC_OK,
            Err(e) => code(&e),
        }
    })
}

/// Change the passphrase (re-wraps the data key; records are not re-encrypted).
#[no_mangle]
pub unsafe extern "C" fn sc_store_change_passphrase(s: *mut ScStore, old: *const c_char, new: *const c_char) -> c_int {
    guard(SC_ERR_PANIC, || {
        let (Some(s), Some(old), Some(new)) = (s.as_mut(), cstr(old), cstr(new)) else { return SC_ERR_ARG };
        match s.0.change_passphrase(old, new, KdfParams::default()) {
            Ok(()) => SC_OK,
            Err(e) => code(&e),
        }
    })
}

/// Close the store and wipe the key from memory.
#[no_mangle]
pub unsafe extern "C" fn sc_store_close(s: *mut ScStore) {
    if !s.is_null() {
        drop(Box::from_raw(s));
    }
}

#[no_mangle]
pub unsafe extern "C" fn sc_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(CString::from_raw(s));
    }
}

#[cfg(test)]
mod ffi_tests {
    use super::*;

    fn c(s: &str) -> CString {
        CString::new(s).unwrap()
    }

    #[test]
    fn c_abi_roundtrip() {
        let dir = std::env::temp_dir().join(format!("sc-ffi-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = c(dir.join("h.corvus").to_str().unwrap());
        let (pw, pw2) = (c("correct horse"), c("battery staple"));
        let rec = |t: &str| c(&format!(r#"{{"t":"m","c":"+49","ts":1,"s":"A","x":"{t}","o":false}}"#));

        unsafe {
            assert_eq!(sc_store_exists(path.as_ptr()), 0);
            let mut err = -1;
            let s = sc_store_create(path.as_ptr(), pw.as_ptr(), &mut err);
            assert!(!s.is_null() && err == SC_OK);
            assert_eq!(sc_store_append(s, rec("one").as_ptr()), SC_OK);
            assert_eq!(sc_store_append(s, rec("two").as_ptr()), SC_OK);
            assert_eq!(sc_store_append(s, c("not json").as_ptr()), SC_ERR_ARG);
            sc_store_close(s);
            assert_eq!(sc_store_exists(path.as_ptr()), 1);

            let mut out: *mut c_char = std::ptr::null_mut();
            let bad = sc_store_open(path.as_ptr(), pw2.as_ptr(), &mut out, &mut err);
            assert!(bad.is_null() && err == SC_ERR_WRONG_PASSPHRASE);

            let s = sc_store_open(path.as_ptr(), pw.as_ptr(), &mut out, &mut err);
            assert!(!s.is_null() && err == SC_OK);
            let json = CStr::from_ptr(out).to_str().unwrap().to_string();
            sc_string_free(out);
            assert!(json.contains("one") && json.contains("two"));

            assert_eq!(sc_store_change_passphrase(s, pw.as_ptr(), pw2.as_ptr()), SC_OK);
            assert_eq!(sc_store_rewrite(s, c(&format!("[{}]", rec("only").to_str().unwrap())).as_ptr()), SC_OK);
            sc_store_close(s);

            let s = sc_store_open(path.as_ptr(), pw2.as_ptr(), &mut out, &mut err);
            assert!(!s.is_null());
            let json = CStr::from_ptr(out).to_str().unwrap().to_string();
            sc_string_free(out);
            assert!(json.contains("only") && !json.contains("one"));
            sc_store_close(s);
        }
    }
}
