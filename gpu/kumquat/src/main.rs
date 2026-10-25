//! The rootless engine's GPU server: guest gfxstream-vk connects to it
//! over kumquat's socket, and it renders through the host's Vulkan.
//!
//! Usage: vine_kumquat SOCKET

use std::ffi::CStr;
use std::os::raw::c_int;

use kumquat_virtio::kumquat::KumquatBuilder;

#[repr(C)]
struct NativeHandle {
    version: c_int,
    num_fds: c_int,
    num_ints: c_int,
    data: [c_int; 0],
}

type GetNativeHandle = unsafe extern "C" fn(*const libc::c_void) -> *const NativeHandle;

fn get_native_handle() -> Option<GetNativeHandle> {
    static SYM: std::sync::OnceLock<Option<GetNativeHandle>> = std::sync::OnceLock::new();
    *SYM.get_or_init(|| unsafe {
        let lib = libc::dlopen(c"libnativewindow.so".as_ptr(), libc::RTLD_NOW);
        if lib.is_null() {
            eprintln!("vine_kumquat: dlopen libnativewindow.so: {}", CStr::from_ptr(libc::dlerror()).to_string_lossy());
            return None;
        }
        let sym = libc::dlsym(lib, c"AHardwareBuffer_getNativeHandle".as_ptr());
        if sym.is_null() {
            return None;
        }
        Some(std::mem::transmute::<*mut libc::c_void, GetNativeHandle>(sym))
    })
}

/// Host-visible memory is an AHB; the guest maps its gralloc dmabuf.
fn ahb_export(ahb: i64) -> Result<i32, ()> {
    let get = get_native_handle().ok_or(())?;
    let nh = unsafe { get(ahb as *const libc::c_void) };
    if nh.is_null() || unsafe { (*nh).num_fds } < 1 {
        return Err(());
    }
    let fd = unsafe { *(*nh).data.as_ptr() };
    let dup = unsafe { libc::dup(fd) };
    if dup < 0 { Err(()) } else { Ok(dup) }
}

fn main() {
    let socket = match std::env::args().nth(1) {
        Some(s) => s,
        None => {
            eprintln!("usage: vine_kumquat SOCKET");
            std::process::exit(2);
        }
    };
    let _ = kumquat_virtio::rutabaga_gfx::set_ahb_export_callback(ahb_export);
    let _ = std::fs::remove_file(&socket);
    let mut kumquat = match KumquatBuilder::new()
        .set_capset_names("gfxstream-vulkan".into())
        // Needs /dev/udmabuf, which apps may not open; FATAL when it fails
        .set_renderer_features("VulkanAllocateHostVisibleAsUdmabuf:disabled".into())
        .set_gpu_socket(Some(socket.clone().into()))
        .build()
    {
        Ok(k) => k,
        Err(e) => {
            eprintln!("vine_kumquat: {:?}", e);
            std::process::exit(1);
        }
    };
    eprintln!("vine_kumquat: listening on {}", socket);
    loop {
        if let Err(e) = kumquat.run() {
            eprintln!("vine_kumquat: {:?}", e);
            std::process::exit(1);
        }
    }
}
