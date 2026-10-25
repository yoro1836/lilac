//! The rootless engine's GPU server: guest gfxstream-vk connects to it
//! over kumquat's socket, and it renders through the host's Vulkan.
//!
//! Usage: vine_kumquat GPU_SOCKET GRALLOC_SOCKET
//!
//! The guest's gralloc allocates its buffers on GRALLOC_SOCKET (see the
//! wayondroid patch to the kumquat server).

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

/// kumquat and rutabaga log through `log`; send it to our log file
struct StderrLog;

impl log::Log for StderrLog {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Warn
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            eprintln!("vine_kumquat: {} {}", r.level(), r.args());
        }
    }
    fn flush(&self) {}
}

static LOGGER: StderrLog = StderrLog;

fn main() {
    let _ = log::set_logger(&LOGGER).map(|()| log::set_max_level(log::LevelFilter::Warn));
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: vine_kumquat GPU_SOCKET GRALLOC_SOCKET");
        std::process::exit(2);
    }
    let socket = args[1].clone();
    let _ = kumquat_virtio::rutabaga_gfx::set_ahb_export_callback(ahb_export);
    let _ = std::fs::remove_file(&socket);
    let mut kumquat = match KumquatBuilder::new()
        .set_capset_names("gfxstream-vulkan".into())
        // Needs /dev/udmabuf, which apps may not open; FATAL when it fails
        .set_renderer_features("VulkanAllocateHostVisibleAsUdmabuf:disabled".into())
        .set_gpu_socket(Some(socket.clone()))
        .set_gralloc_socket(Some(args[2].clone()))
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
