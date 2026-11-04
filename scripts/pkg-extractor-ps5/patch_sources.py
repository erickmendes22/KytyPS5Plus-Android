#!/usr/bin/env python3
"""Integrate native PS5 FPKG extraction (GPL-3.0-or-later) into Android app."""
from pathlib import Path
import sys
app, rust = [Path(a) for a in sys.argv[1:3]]
assert (app/'app/src/main/java/com/example/pkgextractor/Engine.kt').exists()
assert (rust/'src/main.rs').exists()

def swap(p, old, new):
    s=p.read_text()
    if old not in s: raise RuntimeError(f'Anchor changed: {p}: {old[:80]}')
    p.write_text(s.replace(old,new,1))

swap(rust/'Cargo.toml','[dependencies]\n','[lib]\ncrate-type = ["cdylib"]\n\n[dependencies]\njni = "0.21"\n')
code=(rust/'src/main.rs').read_text()
for a,b in [
('    verbose: bool,\n}', '    verbose: bool,\n    raw: bool,\n}'),
('        verbose: false,\n','        verbose: false,\n        raw: false,\n'),
('            Some("--list") => options.list = true,','            Some("--list") => options.list = true,\n            Some("--raw") => options.raw = true,')]:
    if a not in code: raise RuntimeError('Rust source changed: '+a)
    code=code.replace(a,b,1)
raw = """    if options.raw {
        // Keep original PS5 files, without AnyPS5 ELF relinking.
        let app0 = options.output.as_deref().expect("output checked").join("app0");
        create_dir(&app0.join("sce_sys"))?;
        for cnt_file in &cnt.files {
            write_file(&join_inside(&app0.join("sce_sys"), &cnt_file.name)?, &cnt_file.bytes)?;
        }
        status.phase(Phase::Copying, total, files.len() as u64);
        let all_files: Vec<&InnerFile> = files.iter().collect();
        let written = copy_resources(&all_files, options.jobs, started, &status, |file, _, failed, progress| {
            copy_one(&image, file, &app0, failed, progress)
        })?;
        status.line(format!("PS5 extraction complete: {} files, {} bytes", files.len(), written));
        status.finish();
        return Ok(());
    }

"""
anchor = '    let out = options.output.as_deref().expect("checked in parse_args");'
if anchor not in code: raise RuntimeError('Missing PS5 raw insertion point')
code=code.replace(anchor,raw+anchor,1)
(rust/'src/main.rs').write_text(code)
bridge = """
static PS5_CANCEL: AtomicBool = AtomicBool::new(false);

#[unsafe(no_mangle)]
pub extern "system" fn Java_com_example_pkgextractor_NativeBridge_cancelPs5(
    _env: jni::JNIEnv, _this: jni::objects::JObject,
) {
    PS5_CANCEL.store(true, Ordering::Relaxed);
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_com_example_pkgextractor_NativeBridge_extractPs5(
    mut env: jni::JNIEnv, _this: jni::objects::JObject,
    input: jni::objects::JString, output: jni::objects::JString,
) -> jni::sys::jstring {
    let path: String = match env.get_string(&input) {
        Ok(v) => v.into(),
        Err(e) => return env.new_string(format!("Invalid input: {e}"))
            .map(|v| v.into_raw()).unwrap_or(std::ptr::null_mut()),
    };
    let dest: String = match env.get_string(&output) {
        Ok(v) => v.into(),
        Err(e) => return env.new_string(format!("Invalid output: {e}"))
            .map(|v| v.into_raw()).unwrap_or(std::ptr::null_mut()),
    };
    PS5_CANCEL.store(false, Ordering::Relaxed);
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        run(&Options {
            package: PathBuf::from(path),
            output: Some(PathBuf::from(dest)),
            list: false,
            executables_only: false,
            relink: None,
            windows: false,
            only: Vec::new(),
            module_dirs: Vec::new(),
            jobs: 3,
            verbose: false,
            raw: true,
        })
    }));
    let message = match result {
        Ok(Ok(())) => String::new(),
        Ok(Err(e)) => e.to_string(),
        Err(_) => "PS5 extraction engine panicked".to_string(),
    };
    env.new_string(message).map(|v| v.into_raw()).unwrap_or(std::ptr::null_mut())
}
"""
(rust/'src/lib.rs').write_text(code+bridge)
swap(rust/'src/source.rs',
'    pub fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> Result<()> {\n',
'    pub fn read_exact_at(&self, buf: &mut [u8], offset: u64) -> Result<()> {\n        if crate::PS5_CANCEL.load(std::sync::atomic::Ordering::Relaxed) {\n            return Err(Error::run("Cancelled"));\n        }\n')
native=app/'app/src/main/java/com/example/pkgextractor/NativeBridge.kt'
swap(native, 'init { System.loadLibrary("pkgextract") }',
    'init { System.loadLibrary("pkgextract"); System.loadLibrary("ps5extract") }')
swap(native, '    external fun extract(pkgPath: String, outDir: String, progress: Progress): String',
    '    external fun extract(pkgPath: String, outDir: String, progress: Progress): String\n    external fun extractPs5(pkgPath: String, outDir: String): String\n    external fun cancelPs5()')
engine=app/'app/src/main/java/com/example/pkgextractor/Engine.kt'
s=engine.read_text()
s=s.replace('import java.io.File\n','import java.io.File\nimport java.io.FileInputStream\n',1)
old='    fun cancel() { cancelled = true; fire() }'
assert old in s
s=s.replace(old,'    fun cancel() { cancelled = true; try { NativeBridge.cancelPs5() } catch (_: Throwable) {}; fire() }',1)
anchor='    fun start(ctx: Context, uri: Uri, name: String, out: File, coolMode: Boolean): Boolean {'
assert anchor in s
routing = """    private fun isPs5(path: String): Boolean = try {
        FileInputStream(path).use { f ->
            val h = ByteArray(6)
            f.read(h) == 6 && h[0] == 0x7F.toByte() &&
                ((h[1] == 'F'.code.toByte() && h[2] == 'I'.code.toByte() && h[3] == 'H'.code.toByte()) ||
                 (h[1] == 'C'.code.toByte() && h[2] == 'N'.code.toByte() && h[3] == 'T'.code.toByte()))
        }
    } catch (_: Throwable) { false }

    private fun extractRouted(path: String, dest: String, progress: NativeBridge.Progress): String {
        if (!isPs5(path)) return NativeBridge.extract(path, dest, progress)
        phase = "PS5 FPKG: extracting PFS / Kraken"
        resultPath = "$dest/app0"
        fire()
        val result = NativeBridge.extractPs5(path, dest)
        return if (cancelled) "Cancelled" else result
    }

"""
s=s.replace(anchor,routing+anchor,1)
for old,new in [
('NativeBridge.extract(direct, out.absolutePath, progress)','extractRouted(direct, out.absolutePath, progress)'),
('NativeBridge.extract("/proc/self/fd/${pfd.fd}", out.absolutePath, progress)','extractRouted("/proc/self/fd/${pfd.fd}", out.absolutePath, progress)')]:
    assert old in s, old
    s=s.replace(old,new,1)
engine.write_text(s)
swap(app/'app/build.gradle.kts',
'    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }',
'    externalNativeBuild { cmake { path = file("src/main/cpp/CMakeLists.txt") } }\n    packaging { jniLibs { useLegacyPackaging = true } }')
print('PS5 FPKG JNI integration installed')
