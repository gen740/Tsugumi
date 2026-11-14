//! Safe, call-scoped access to the Card ABI. Only this module handles pointers.
use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::marker::PhantomData;
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::rc::Rc;
use std::path::Path;

#[derive(Debug)]
pub struct Error {
    pub kind: &'static str,
    pub message: String,
}
impl Error {
    pub fn new(kind: &'static str, message: impl Into<String>) -> Self {
        Self {
            kind,
            message: message.into(),
        }
    }
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{}: {}", self.kind, self.message)
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::new("worker_failed", error.to_string())
    }
}
impl From<flatbuffers::InvalidFlatbuffer> for Error {
    fn from(error: flatbuffers::InvalidFlatbuffer) -> Self {
        Self::new("type_mismatch", error.to_string())
    }
}
pub type Result<T> = std::result::Result<T, Error>;

#[link(name = "tsugumi_rust_host", kind = "static")]
unsafe extern "C" {
    fn tsugumi_rs_line(
        call: *const c_void,
        id: *const c_char,
        label: *const c_char,
        x: *const f64,
        y: *const f64,
        count: usize,
    ) -> c_int;
    fn tsugumi_rs_points(
        call: *const c_void,
        x: *const f32,
        y: *const f32,
        z: *const f32,
        colors: *const u32,
        count: usize,
    ) -> c_int;
    fn tsugumi_rs_info(call: *const c_void, field: u32) -> *const c_char;
    fn tsugumi_rs_invocation(call: *const c_void) -> u64;
    fn tsugumi_rs_finish_outputs(call: *const c_void) -> c_int;
    fn tsugumi_rs_check(call: *const c_void) -> c_int;
    fn tsugumi_rs_input(call: *const c_void, name: *const c_char, size: *mut usize) -> *const u8;
    fn tsugumi_rs_params(call: *const c_void, size: *mut usize) -> *const u8;
    fn tsugumi_rs_emit(
        call: *const c_void,
        port: *const c_char,
        bytes: *const u8,
        size: usize,
    ) -> c_int;
    fn tsugumi_rs_log(call: *const c_void, text: *const c_char);
    fn tsugumi_rs_fail(call: *const c_void, kind: *const c_char, text: *const c_char);
    fn tsugumi_rs_cancelled(call: *const c_void) -> c_int;
    fn tsugumi_rs_remaining(call: *const c_void) -> i64;
    fn tsugumi_rs_text(call: *const c_void, text: *const c_char, markdown: c_int) -> c_int;
    fn tsugumi_rs_scalar(call: *const c_void, value: f64, label: *const c_char) -> c_int;
    fn tsugumi_rs_module(call: *const c_void, name: *const c_char, json: *const c_char) -> c_int;
    fn tsugumi_rs_image(
        call: *const c_void,
        media: *const c_char,
        bytes: *const u8,
        size: usize,
        alt: *const c_char,
    ) -> c_int;
    fn tsugumi_rs_table(
        call: *const c_void,
        columns: *const *const c_char,
        count: usize,
        cells: *const *const c_char,
        rows: usize,
    ) -> c_int;
}

fn string(value: &str) -> Result<CString> {
    CString::new(value).map_err(|_| Error::new("output_invalid", "text contains a NUL byte"))
}
fn accepted(code: c_int) -> Result<()> {
    if code == 0 {
        Ok(())
    } else {
        Err(Error::new(
            "output_invalid",
            "the worker rejected the value",
        ))
    }
}

/// Valid only within the current invocation. It cannot be sent to another thread.
/// Input/params views borrow this call; emitted output bytes are copied by the host.
#[doc(hidden)]
pub struct Call<'call> {
    raw: *const c_void,
    lifetime: PhantomData<&'call ()>,
    thread: PhantomData<Rc<()>>,
}
impl Call<'_> {
    pub fn finish_outputs(&self) -> Result<()> { accepted(unsafe { tsugumi_rs_finish_outputs(self.raw) }) }
    fn info(&self, field: u32) -> &str {
        let value = unsafe { tsugumi_rs_info(self.raw, field) };
        if value.is_null() {
            ""
        } else {
            unsafe { CStr::from_ptr(value) }.to_str().unwrap_or("")
        }
    }
    pub fn run_id(&self) -> &str {
        self.info(0)
    }
    pub fn card(&self) -> &str {
        self.info(1)
    }
    pub fn working_directory(&self) -> &Path {
        Path::new(self.info(2))
    }
    pub fn temporary_directory(&self) -> &Path {
        Path::new(self.info(3))
    }
    pub fn output_directory(&self) -> &Path {
        Path::new(self.info(4))
    }
    pub fn invocation(&self) -> u64 {
        unsafe { tsugumi_rs_invocation(self.raw) }
    }

    pub(crate) fn input_bytes(&self, port: &str) -> Result<Option<&[u8]>> {
        let port = string(port)?;
        let mut size = 0;
        // SAFETY: Call is made only for a validated live host call; the host owns its slices.
        let bytes = unsafe { tsugumi_rs_input(self.raw, port.as_ptr(), &mut size) };
        Ok(if bytes.is_null() {
            None
        } else {
            Some(unsafe { std::slice::from_raw_parts(bytes, size) })
        })
    }
    pub(crate) fn params_bytes(&self) -> &[u8] {
        let mut size = 0;
        let bytes = unsafe { tsugumi_rs_params(self.raw, &mut size) };
        if bytes.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(bytes, size) }
        }
    }
    pub fn cancelled(&self) -> bool {
        unsafe { tsugumi_rs_cancelled(self.raw) != 0 }
    }
    /// None means there is no deadline; otherwise milliseconds remaining.
    pub fn remaining_ms(&self) -> Option<u64> {
        u64::try_from(unsafe { tsugumi_rs_remaining(self.raw) }).ok()
    }
    pub fn log(&self, text: &str) -> Result<()> {
        let text = string(text)?;
        unsafe { tsugumi_rs_log(self.raw, text.as_ptr()) };
        Ok(())
    }
    pub(crate) fn emit(&self, port: &str, bytes: &[u8]) -> Result<()> {
        let port = string(port)?;
        accepted(unsafe { tsugumi_rs_emit(self.raw, port.as_ptr(), bytes.as_ptr(), bytes.len()) })
    }
    pub fn text(&self, text: &str) -> Result<()> {
        self.send_text(text, false)
    }
    pub fn markdown(&self, text: &str) -> Result<()> {
        self.send_text(text, true)
    }
    fn send_text(&self, text: &str, markdown: bool) -> Result<()> {
        let text = string(text)?;
        accepted(unsafe { tsugumi_rs_text(self.raw, text.as_ptr(), markdown.into()) })
    }
    pub fn scalar(&self, value: f64, label: &str) -> Result<()> {
        let label = string(label)?;
        accepted(unsafe { tsugumi_rs_scalar(self.raw, value, label.as_ptr()) })
    }
    pub fn module(&self, name: &str, json: &str) -> Result<()> {
        let (name, json) = (string(name)?, string(json)?);
        accepted(unsafe { tsugumi_rs_module(self.raw, name.as_ptr(), json.as_ptr()) })
    }
    pub fn image(&self, media_type: &str, bytes: &[u8], alt: &str) -> Result<()> {
        let (media, alt) = (string(media_type)?, string(alt)?);
        accepted(unsafe {
            tsugumi_rs_image(
                self.raw,
                media.as_ptr(),
                bytes.as_ptr(),
                bytes.len(),
                alt.as_ptr(),
            )
        })
    }
    pub fn line_chart(&self, id: &str, label: &str, x: &[f64], y: &[f64]) -> Result<()> {
        if x.len() != y.len() {
            return Err(Error::new(
                "output_invalid",
                "line coordinates must have equal lengths",
            ));
        }
        let (id, label) = (string(id)?, string(label)?);
        accepted(unsafe {
            tsugumi_rs_line(
                self.raw,
                id.as_ptr(),
                label.as_ptr(),
                x.as_ptr(),
                y.as_ptr(),
                x.len(),
            )
        })
    }
    pub fn points(&self, x: &[f32], y: &[f32], z: &[f32], colors: Option<&[u32]>) -> Result<()> {
        if x.len() != y.len() || x.len() != z.len() || colors.is_some_and(|c| c.len() != x.len()) {
            return Err(Error::new(
                "output_invalid",
                "point coordinates and colors must have equal lengths",
            ));
        }
        accepted(unsafe {
            tsugumi_rs_points(
                self.raw,
                x.as_ptr(),
                y.as_ptr(),
                z.as_ptr(),
                colors.map_or(std::ptr::null(), |c| c.as_ptr()),
                x.len(),
            )
        })
    }
    pub fn table(&self, columns: &[&str], rows: &[Vec<String>]) -> Result<()> {
        if rows.iter().any(|r| r.len() != columns.len()) {
            return Err(Error::new(
                "output_invalid",
                "table rows must match columns",
            ));
        }
        let columns: Vec<_> = columns.iter().map(|s| string(s)).collect::<Result<_>>()?;
        let cells: Vec<_> = rows
            .iter()
            .flatten()
            .map(|s| string(s))
            .collect::<Result<_>>()?;
        let column_ptrs: Vec<_> = columns.iter().map(|s| s.as_ptr()).collect();
        let cell_ptrs: Vec<_> = cells.iter().map(|s| s.as_ptr()).collect();
        accepted(unsafe {
            tsugumi_rs_table(
                self.raw,
                column_ptrs.as_ptr(),
                columns.len(),
                cell_ptrs.as_ptr(),
                rows.len(),
            )
        })
    }
}

/// Entry point used only by the generated export macro.
/// # Safety
/// `raw` must be the host's valid call pointer for the duration of this function.
#[doc(hidden)]
pub unsafe fn invoke(raw: *const c_void, body: impl FnOnce(&Call<'_>) -> Result<()>) -> c_int {
    // SAFETY: caller is the worker; C checks the version before accessing further fields.
    if unsafe { tsugumi_rs_check(raw) } == 0 {
        return 1;
    }
    let call = Call {
        raw,
        lifetime: PhantomData,
        thread: PhantomData,
    };
    let result = catch_unwind(AssertUnwindSafe(|| body(&call)));
    let error = match result {
        Ok(Ok(())) => return 0,
        Ok(Err(error)) => error,
        Err(panic) => {
            let message = panic
                .downcast_ref::<&str>()
                .copied()
                .or_else(|| panic.downcast_ref::<String>().map(String::as_str))
                .unwrap_or("Rust Card panicked");
            let error = Error::new("worker_failed", message);
            // A custom panic payload may panic again from Drop. The failed
            // worker is discarded, so forget that payload before returning to C.
            std::mem::forget(panic);
            error
        }
    };
    // Error messages may themselves contain NUL; reporting must never panic.
    let kind = CString::new(error.kind.replace('\0', "\\0")).unwrap_or_default();
    let message = CString::new(error.message.replace('\0', "\\0")).unwrap_or_default();
    unsafe {
        tsugumi_rs_log(raw, message.as_ptr());
        tsugumi_rs_fail(raw, kind.as_ptr(), message.as_ptr());
    }
    1
}

pub struct Report<'a>(pub(crate) &'a Call<'a>);
impl<'a> Report<'a> {
    #[doc(hidden)]
    pub fn from_call(call: &'a Call<'a>) -> Self {
        Self(call)
    }
    pub fn log(&self, text: &str) -> Result<()> {
        self.0.log(text)
    }
    pub fn text(&self, text: &str) -> Result<()> {
        self.0.text(text)
    }
    pub fn markdown(&self, text: &str) -> Result<()> {
        self.0.markdown(text)
    }
    pub fn scalar(&self, value: f64, label: &str) -> Result<()> {
        self.0.scalar(value, label)
    }
    pub fn module(&self, name: &str, json: &str) -> Result<()> {
        self.0.module(name, json)
    }
    pub fn image(&self, media_type: &str, bytes: &[u8], alt: &str) -> Result<()> {
        self.0.image(media_type, bytes, alt)
    }
    pub fn line_chart(&self, id: &str, label: &str, x: &[f64], y: &[f64]) -> Result<()> {
        self.0.line_chart(id, label, x, y)
    }
    pub fn points(&self, x: &[f32], y: &[f32], z: &[f32], colors: Option<&[u32]>) -> Result<()> {
        self.0.points(x, y, z, colors)
    }
    pub fn table(&self, columns: &[&str], rows: &[Vec<String>]) -> Result<()> {
        self.0.table(columns, rows)
    }
}

pub struct Run<'a>(&'a Call<'a>);
impl<'a> Run<'a> {
    #[doc(hidden)]
    pub fn from_call(call: &'a Call<'a>) -> Self { Self(call) }
    pub fn run_id(&self) -> &str { self.0.run_id() }
    pub fn card(&self) -> &str { self.0.card() }
    pub fn invocation(&self) -> u64 { self.0.invocation() }
    pub fn working_directory(&self) -> &Path { self.0.working_directory() }
    pub fn temporary_directory(&self) -> &Path { self.0.temporary_directory() }
    pub fn output_directory(&self) -> &Path { self.0.output_directory() }
    pub fn cancelled(&self) -> bool { self.0.cancelled() }
    pub fn remaining_ms(&self) -> Option<u64> { self.0.remaining_ms() }
}
