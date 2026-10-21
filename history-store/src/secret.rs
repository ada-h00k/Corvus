//! Wipe-on-drop wrapper for key material (replaces the `zeroize` crate).
//!
//! The wipe uses volatile writes followed by a compiler fence so the optimizer
//! cannot elide it as a dead store. Like `zeroize`, this cannot reach copies
//! that were made earlier (moves, reallocations, registers, swap).

use std::ops::{Deref, DerefMut};
use std::sync::atomic::{compiler_fence, Ordering};

/// Overwrite `len` bytes at `p` with zeros, guaranteed not to be optimized out.
///
/// # Safety
/// `p` must be valid for writes of `len` bytes.
unsafe fn wipe_raw(p: *mut u8, len: usize) {
    for i in 0..len {
        std::ptr::write_volatile(p.add(i), 0);
    }
    compiler_fence(Ordering::SeqCst);
}

/// Types whose contents can be wiped.
pub trait Wipe {
    fn wipe(&mut self);
}

impl<const N: usize> Wipe for [u8; N] {
    fn wipe(&mut self) {
        // SAFETY: the array is N valid, writable bytes.
        unsafe { wipe_raw(self.as_mut_ptr(), N) }
    }
}

impl Wipe for Vec<u8> {
    fn wipe(&mut self) {
        // Wipe the whole allocation (including spare capacity), then truncate.
        // SAFETY: `capacity()` bytes starting at `as_mut_ptr()` are allocated.
        unsafe { wipe_raw(self.as_mut_ptr(), self.capacity()) }
        self.clear();
    }
}

/// Argon2 working memory (derived from the passphrase), wiped on drop.
#[derive(Default)]
pub struct BlockMem(pub Vec<argon2::Block>);

impl Wipe for BlockMem {
    fn wipe(&mut self) {
        // SAFETY: `Block` is a plain array of u64 words (all-zero is valid) and
        // the vector owns `capacity()` blocks of allocated memory.
        unsafe { wipe_raw(self.0.as_mut_ptr().cast(), self.0.capacity() * std::mem::size_of::<argon2::Block>()) }
        self.0.clear();
    }
}

/// Owns a secret and wipes it when dropped.
pub struct Zeroizing<T: Wipe>(T);

impl<T: Wipe> Zeroizing<T> {
    pub fn new(v: T) -> Self {
        Zeroizing(v)
    }
}

impl<T: Wipe> Deref for Zeroizing<T> {
    type Target = T;
    fn deref(&self) -> &T {
        &self.0
    }
}

impl<T: Wipe> DerefMut for Zeroizing<T> {
    fn deref_mut(&mut self) -> &mut T {
        &mut self.0
    }
}

impl<T: Wipe> Drop for Zeroizing<T> {
    fn drop(&mut self) {
        self.0.wipe();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn array_is_wiped() {
        let mut a = [0xAAu8; 32];
        a.wipe();
        assert_eq!(a, [0u8; 32]);
    }

    #[test]
    fn vec_is_wiped_including_spare_capacity() {
        let mut v = Vec::with_capacity(64);
        v.extend_from_slice(&[0xAA; 40]);
        let p = v.as_ptr();
        let cap = v.capacity();
        v.wipe();
        assert!(v.is_empty());
        // SAFETY: allocation is still owned by `v`; reading initialized-by-wipe bytes.
        let bytes = unsafe { std::slice::from_raw_parts(p, cap) };
        assert!(bytes.iter().all(|&b| b == 0));
    }

    #[test]
    fn wipes_on_drop() {
        let mut slot = Box::new(Zeroizing::new([0x55u8; 32]));
        let p: *const u8 = slot.as_ptr();
        // Run Drop in place without freeing, so the memory can be inspected.
        // SAFETY: `slot` is not used as a Zeroizing again after drop_in_place.
        unsafe { std::ptr::drop_in_place(&mut *slot) };
        let bytes = unsafe { std::slice::from_raw_parts(p, 32) };
        assert!(bytes.iter().all(|&b| b == 0));
        std::mem::forget(slot);
    }

    #[test]
    fn block_mem_is_wiped() {
        let mut m = BlockMem::default();
        m.0.resize(8, argon2::Block::default());
        // SAFETY: test-only poke at the words of a plain-data block.
        let p = m.0.as_mut_ptr().cast::<u8>();
        unsafe { std::ptr::write_bytes(p, 0xAA, 8 * std::mem::size_of::<argon2::Block>()) };
        let len = m.0.capacity() * std::mem::size_of::<argon2::Block>();
        m.wipe();
        let bytes = unsafe { std::slice::from_raw_parts(p, len) };
        assert!(bytes.iter().all(|&b| b == 0));
    }

    #[test]
    fn deref_gives_access() {
        let mut z = Zeroizing::new([1u8, 2, 3]);
        z[0] = 9;
        assert_eq!(&z[..], &[9, 2, 3]);
    }
}
