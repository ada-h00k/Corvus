//! Operating-system random bytes (replaces the `getrandom` crate).
//!
//! Only the platforms Corvus ships for are implemented, each through the
//! kernel's CSPRNG entry point. There is no user-space fallback generator:
//! on any failure an error is returned and the caller must abort the
//! operation (fail closed).
//!
//! * Linux / Android: `getrandom(2)` (blocks until the pool is initialised);
//!   `/dev/urandom` only if the kernel lacks the syscall (`ENOSYS`).
//! * macOS / iOS: `getentropy(2)`, at most 256 bytes per call.
//! * Windows (10+): `BCryptGenRandom` with the system-preferred RNG
//!   (`bcrypt` is already linked by CMakeLists.txt).
//! * Other Unix: `/dev/urandom`.

use std::io;

/// Fill `buf` with cryptographically secure random bytes.
pub fn fill(buf: &mut [u8]) -> io::Result<()> {
    if buf.is_empty() {
        return Ok(());
    }
    sys::fill(buf)
}

#[cfg(any(target_os = "linux", target_os = "android"))]
mod sys {
    use std::ffi::{c_uint, c_void};
    use std::io;

    extern "C" {
        // glibc >= 2.25, musl, bionic.
        fn getrandom(buf: *mut c_void, buflen: usize, flags: c_uint) -> isize;
    }

    const EINTR: i32 = 4;
    const ENOSYS: i32 = 38;

    pub fn fill(mut buf: &mut [u8]) -> io::Result<()> {
        while !buf.is_empty() {
            // SAFETY: `buf` is valid for `buf.len()` writable bytes.
            let n = unsafe { getrandom(buf.as_mut_ptr().cast(), buf.len(), 0) };
            if n < 0 {
                let e = io::Error::last_os_error();
                match e.raw_os_error() {
                    Some(EINTR) => continue,
                    Some(ENOSYS) => return super::urandom(buf),
                    _ => return Err(e),
                }
            }
            let n = n as usize;
            if n == 0 || n > buf.len() {
                return Err(io::Error::new(io::ErrorKind::Other, "getrandom returned an invalid length"));
            }
            buf = &mut buf[n..];
        }
        Ok(())
    }
}

#[cfg(any(target_os = "macos", target_os = "ios"))]
mod sys {
    use std::ffi::{c_int, c_void};
    use std::io;

    extern "C" {
        fn getentropy(buf: *mut c_void, buflen: usize) -> c_int;
    }

    pub fn fill(buf: &mut [u8]) -> io::Result<()> {
        for chunk in buf.chunks_mut(256) {
            // SAFETY: `chunk` is valid for `chunk.len()` (<= 256) writable bytes.
            if unsafe { getentropy(chunk.as_mut_ptr().cast(), chunk.len()) } != 0 {
                return Err(io::Error::last_os_error());
            }
        }
        Ok(())
    }
}

#[cfg(windows)]
mod sys {
    use std::ffi::c_void;
    use std::io;

    #[link(name = "bcrypt")]
    extern "system" {
        fn BCryptGenRandom(algorithm: *mut c_void, buffer: *mut u8, count: u32, flags: u32) -> i32;
    }

    const BCRYPT_USE_SYSTEM_PREFERRED_RNG: u32 = 0x0000_0002;

    pub fn fill(buf: &mut [u8]) -> io::Result<()> {
        for chunk in buf.chunks_mut(1 << 20) {
            // SAFETY: `chunk` is valid for `chunk.len()` writable bytes (< 2^32);
            // a null algorithm handle plus the flag selects the system RNG.
            let status = unsafe {
                BCryptGenRandom(std::ptr::null_mut(), chunk.as_mut_ptr(), chunk.len() as u32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)
            };
            if status != 0 {
                return Err(io::Error::new(io::ErrorKind::Other, format!("BCryptGenRandom failed: 0x{status:08x}")));
            }
        }
        Ok(())
    }
}

#[cfg(all(unix, not(any(target_os = "linux", target_os = "android", target_os = "macos", target_os = "ios"))))]
mod sys {
    use std::io;

    pub fn fill(buf: &mut [u8]) -> io::Result<()> {
        super::urandom(buf)
    }
}

#[cfg(not(any(unix, windows)))]
compile_error!("history-store: no OS random source implemented for this target (see src/rng.rs)");

#[cfg(unix)]
fn urandom(buf: &mut [u8]) -> io::Result<()> {
    use std::io::Read;
    std::fs::File::open("/dev/urandom")?.read_exact(buf)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn empty_is_ok() {
        fill(&mut []).unwrap();
    }

    #[test]
    fn fills_and_differs() {
        let (mut a, mut b) = ([0u8; 32], [0u8; 32]);
        fill(&mut a).unwrap();
        fill(&mut b).unwrap();
        assert_ne!(a, [0u8; 32]);
        assert_ne!(a, b);
    }

    #[test]
    fn large_buffers_cover_every_chunk() {
        // > 256 (getentropy limit) and not a multiple of it.
        let mut big = vec![0u8; 5000];
        fill(&mut big).unwrap();
        for chunk in big.chunks(250) {
            assert!(chunk.iter().any(|&b| b != 0), "a whole chunk stayed zero");
        }
    }

    #[test]
    fn rough_bit_balance() {
        let mut big = vec![0u8; 1 << 16];
        fill(&mut big).unwrap();
        let ones: u32 = big.iter().map(|b| b.count_ones()).sum();
        let total = (big.len() * 8) as u32;
        // 524288 bits: sigma ~ 362; +-4000 is > 10 sigma. Catches stuck/biased output only.
        assert!(ones.abs_diff(total / 2) < 4000, "ones = {ones}");
    }
}
