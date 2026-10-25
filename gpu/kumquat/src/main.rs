//! The rootless engine's GPU server: guest gfxstream-vk connects to it
//! over kumquat's socket, and it renders through the host's Vulkan.
//!
//! Usage: vine_kumquat GPU_SOCKET GRALLOC_SOCKET [AHB_SOCKET]
//!
//! The guest's gralloc allocates its buffers on GRALLOC_SOCKET (see the
//! wayondroid patch to the kumquat server). AHB_SOCKET, an abstract
//! SOCK_SEQPACKET name, hands the app a resource's AHardwareBuffer so it
//! can show guest frames without copying them: the app sends a u32
//! resource id, gets a u32 status (0: ok) and then the buffer, in
//! AHardwareBuffer_sendHandleToUnixSocket's format.

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
type SendHandle = unsafe extern "C" fn(*const libc::c_void, c_int) -> c_int;
type Release = unsafe extern "C" fn(*mut libc::c_void);

extern "C" {
    // libgfxstream_backend (gfxstream patch 02): a ColorBuffer's AHB, acquired
    fn tawc_gfxstream_lookup_ahb(handle: u32) -> *mut libc::c_void;
}

fn nativewindow(name: &CStr) -> *mut libc::c_void {
    static LIB: std::sync::OnceLock<usize> = std::sync::OnceLock::new();
    let lib = *LIB.get_or_init(|| unsafe {
        let lib = libc::dlopen(c"libnativewindow.so".as_ptr(), libc::RTLD_NOW);
        if lib.is_null() {
            eprintln!("vine_kumquat: dlopen libnativewindow.so: {}", CStr::from_ptr(libc::dlerror()).to_string_lossy());
        }
        lib as usize
    });
    if lib == 0 {
        return std::ptr::null_mut();
    }
    unsafe { libc::dlsym(lib as *mut libc::c_void, name.as_ptr()) }
}

fn get_native_handle() -> Option<GetNativeHandle> {
    let sym = nativewindow(c"AHardwareBuffer_getNativeHandle");
    (!sym.is_null()).then(|| unsafe { std::mem::transmute::<*mut libc::c_void, GetNativeHandle>(sym) })
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

/// Answers one app connection on the AHB socket until it closes
fn serve_ahb(fd: c_int, send: SendHandle, release: Release) {
    loop {
        let mut id = [0u8; 4];
        let n = unsafe { libc::recv(fd, id.as_mut_ptr().cast(), 4, 0) };
        if n != 4 {
            break;
        }
        let ahb = unsafe { tawc_gfxstream_lookup_ahb(u32::from_le_bytes(id)) };
        let status: u32 = if ahb.is_null() { 1 } else { 0 };
        let sent = unsafe { libc::send(fd, status.to_le_bytes().as_ptr().cast(), 4, libc::MSG_NOSIGNAL) } == 4
            && (ahb.is_null() || unsafe { send(ahb, fd) } == 0);
        if !ahb.is_null() {
            unsafe { release(ahb) };
        }
        if !sent {
            break;
        }
    }
    unsafe { libc::close(fd) };
}

fn listen_ahb(name: &str) {
    let send = nativewindow(c"AHardwareBuffer_sendHandleToUnixSocket");
    let release = nativewindow(c"AHardwareBuffer_release");
    if send.is_null() || release.is_null() {
        eprintln!("vine_kumquat: no AHardwareBuffer socket API, frames go through the CPU");
        return;
    }
    let send = unsafe { std::mem::transmute::<*mut libc::c_void, SendHandle>(send) };
    let release = unsafe { std::mem::transmute::<*mut libc::c_void, Release>(release) };
    let fd = unsafe { libc::socket(libc::AF_UNIX, libc::SOCK_SEQPACKET | libc::SOCK_CLOEXEC, 0) };
    let mut sa: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    sa.sun_family = libc::AF_UNIX as libc::sa_family_t;
    let bytes = name.as_bytes();
    if fd < 0 || bytes.len() + 1 > sa.sun_path.len() {
        eprintln!("vine_kumquat: cannot make the AHB socket");
        return;
    }
    // Abstract: a leading NUL, no terminator
    for (i, b) in bytes.iter().enumerate() {
        sa.sun_path[i + 1] = *b as libc::c_char;
    }
    let len = (std::mem::size_of::<libc::sa_family_t>() + 1 + bytes.len()) as libc::socklen_t;
    if unsafe { libc::bind(fd, (&sa as *const libc::sockaddr_un).cast(), len) } != 0
        || unsafe { libc::listen(fd, 4) } != 0
    {
        eprintln!("vine_kumquat: AHB socket {}: {}", name, std::io::Error::last_os_error());
        unsafe { libc::close(fd) };
        return;
    }
    std::thread::spawn(move || loop {
        let c = unsafe { libc::accept4(fd, std::ptr::null_mut(), std::ptr::null_mut(), libc::SOCK_CLOEXEC) };
        if c >= 0 {
            std::thread::spawn(move || serve_ahb(c, send, release));
        } else if std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
            eprintln!("vine_kumquat: AHB socket accept: {}", std::io::Error::last_os_error());
            return;
        }
    });
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
    if args.len() != 3 && args.len() != 4 {
        eprintln!("usage: vine_kumquat GPU_SOCKET GRALLOC_SOCKET [AHB_SOCKET]");
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
    if let Some(name) = args.get(3).filter(|n| !n.is_empty()) {
        listen_ahb(name);
    }
    eprintln!("vine_kumquat: listening on {}", socket);
    loop {
        if let Err(e) = kumquat.run() {
            eprintln!("vine_kumquat: {:?}", e);
            std::process::exit(1);
        }
    }
}
