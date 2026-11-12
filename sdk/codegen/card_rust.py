"""Generate a borrowed input / owned output Rust face for one Card contract."""
import json
import os
import re
import sys

KEYWORDS = set('as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while abstract become box do final macro override priv typeof unsized virtual yield try gen'.split())


def snake(name):
    name = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    return re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name).lower()


def member(name):
    name = snake(name)
    return name + '_' if name in KEYWORDS else name


def rust(name, face, owned=False):
    parts = name.split('.')
    return face + '::' + '::'.join([member(p) for p in parts[:-1]] + [parts[-1] + ('T' if owned else "<'a>")])


def alias(name):
    return ''.join(part[:1].upper() + part[1:] for part in name.split('_'))


def main(contract_path, out):
    contract = json.load(open(contract_path))
    os.makedirs(os.path.join(out, 'src'), exist_ok=True)
    lines = ['// Generated from this Card\'s FlatBuffers contract. Do not edit.',
             'extern crate alloc;', 'mod runtime;', 'pub use runtime::{Context, Error, Result};', 'pub use flatbuffers;',
             '#[doc(hidden)] pub use runtime::invoke as __invoke;',
             '#[allow(warnings)] #[path="_inputs_generated.rs"] pub mod input_types;',
             '#[allow(warnings)] #[path="_outputs_generated.rs"] pub mod output_types;',
             '#[allow(warnings)] #[path="_params_generated.rs"] pub mod param_types;',
             'pub type Params<\'a> = param_types::params::Params<\'a>;',
             'pub mod outputs {']
    for port in contract['outputs']:
        lines.append(f"    pub type {alias(port['name'])} = super::{rust(port['type'], 'output_types', True)};")
    lines += ['}', '/// Views borrow the current call; optional ports may be absent.', "pub struct Input<'a> {"]
    if not contract['inputs']:
        lines.append("    lifetime: std::marker::PhantomData<&'a ()>,")
    for port in contract['inputs']:
        typ = rust(port['type'], 'input_types')
        lines.append(f"    pub {member(port['name'])}: {typ if port['required'] else 'Option<' + typ + '>'},")
    lines += ['}', '/// Owned output objects; only Some ports are sent.', '#[derive(Default)]', 'pub struct Output {']
    for port in contract['outputs']:
        lines.append(f"    pub {member(port['name'])}: Option<outputs::{alias(port['name'])}>,")
    lines += ['}', "impl Context<'_> {", "    pub fn params(&self) -> Result<Params<'_>> {",
              '        Ok(flatbuffers::root::<Params>(self.params_bytes())?)', '    }', '}',
              "impl<'a> Input<'a> {", "    pub fn read(ctx: &'a Context<'_>) -> Result<Self> {", '        Ok(Self {']
    if not contract['inputs']:
        lines.append('            lifetime: std::marker::PhantomData,')
    for port in contract['inputs']:
        value = f"ctx.input_bytes({json.dumps(port['name'])})?"
        if port['required']:
            value += f'.ok_or_else(|| Error::new("input_missing", {json.dumps(port["name"])}))?'
            value = f'flatbuffers::root::<{rust(port["type"], "input_types")}>({value})?'
        else:
            value += f'.map(flatbuffers::root::<{rust(port["type"], "input_types")}>) .transpose()?'
        lines.append(f"            {member(port['name'])}: {value},")
    lines += ['        })', '    }', '}', 'impl Output {', '    pub fn emit(self, ctx: &Context<\'_>) -> Result<()> {']
    for port in contract['outputs']:
        lines += [f"        if let Some(value) = self.{member(port['name'])} {{",
                  '            let mut builder = flatbuffers::FlatBufferBuilder::new();',
                  '            let root = value.pack(&mut builder);', '            builder.finish(root, None);',
                  f'            ctx.emit({json.dumps(port["name"])}, builder.finished_data())?;', '        }']
    if not contract['outputs']:
        lines.append('        let _ = ctx;')
    lines += ['        Ok(())', '    }', '}', r'''
/// Export a Card function, optionally followed by an on_run_start hook.
/// Both functions return Result; panic is caught before returning to C.
#[macro_export]
macro_rules! export {
    ($run:path) => {
        $crate::export!($run, |_ctx: &$crate::Context<'_>| -> $crate::Result<()> { Ok(()) });
    };
    ($run:path, $start:expr) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn tsugumi_main(raw: *const std::ffi::c_void) -> std::ffi::c_int {
            // SAFETY: called only by the worker with its live ABI call.
            unsafe { $crate::__invoke(raw, |ctx| $run(ctx, $crate::Input::read(ctx)?)?.emit(ctx)) }
        }
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn tsugumi_run_start(raw: *const std::ffi::c_void) -> std::ffi::c_int {
            unsafe { $crate::__invoke(raw, $start) }
        }
    };
}
''']
    open(os.path.join(out, 'src/lib.rs'), 'w').write('\n'.join(lines))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
