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
             'extern crate alloc;', 'extern crate tsugumi_runtime as runtime;',
             'pub use runtime::{Report, Run, Error, Result};', 'pub extern crate flatbuffers;',
             '#[doc(hidden)] pub use runtime::{invoke as __invoke, Call as __Call, runs_card as __runs_card, run_start as __run_start};',
             '#[allow(warnings)] #[path="_inputs_generated.rs"] pub mod input_types;',
             '#[allow(warnings)] #[path="_outputs_generated.rs"] pub mod output_types;',
             '#[allow(warnings)] #[path="_params_generated.rs"] pub mod param_types;',
             'pub type Params<\'a> = param_types::parameters::Params<\'a>;',
             'pub mod outputs {']
    for port in contract['outputs']:
        lines.append(f"    pub type {alias(port['name'])} = super::{rust(port['type'], 'output_types', True)};")
    lines += ['}', '/// Views borrow the current call; optional ports may be absent.', "pub struct Inputs<'a> {"]
    lines.append("    pub params: Params<'a>,")
    for port in contract['inputs']:
        typ = rust(port['type'], 'input_types')
        lines.append(f"    pub {member(port['name'])}: {typ if port['required'] else 'Option<' + typ + '>'},")
    lines += ['}', '/// Owned output objects; only Some ports are sent.', '#[derive(Default)]', 'pub struct Output {']
    for port in contract['outputs']:
        lines.append(f"    pub {member(port['name'])}: Option<outputs::{alias(port['name'])}>,")
    lines += ['}', "impl<'a> Inputs<'a> {", "    pub fn read(call: &'a __Call<'_>) -> Result<Self> {"]
    lines += ['        Ok(Self {', '            params: flatbuffers::root::<Params>(call.params_bytes())?,']
    for port in contract['inputs']:
        m = member(port['name'])
        name = port['name']
        typ = rust(port["type"], "input_types")
        value = f'call.input(c"{name}").map(flatbuffers::root::<{typ}>).transpose()?'
        if port['required']:
            value += f'.ok_or_else(|| Error::new("input_missing", "{name}"))?'
        lines.append(f'            {m}: {value},')
    lines += ['        })', '    }', '}', 'impl Output {',
              "    pub fn send(self, _call: &__Call<'_>) -> Result<()> {"]
    for port in contract['outputs']:
        m = member(port['name'])
        lines += [f'        if let Some(value) = self.{m} {{',
                  '            let mut builder = flatbuffers::FlatBufferBuilder::new();',
                  '            let root = value.pack(&mut builder);',
                  '            builder.finish(root, None);',
                  f'            _call.output(c"{port["name"]}", builder.finished_data())?;', '        }']
    lines += ['        Ok(())', '    }', '}', r'''
/// Export a Card function, optionally followed by an on_run_start hook.
/// Both functions return Result; panic is caught before returning to C.
#[macro_export]
macro_rules! export {
    ($function:path) => { $crate::export!(@body |inputs, report, _run| $function(inputs, report), |_, _, _| Ok(())); };
    ($function:path, inputs) => { $crate::export!(@body |inputs, _report, _run| $function(inputs), |_, _, _| Ok(())); };
    ($function:path, with_run) => { $crate::export!(@body |inputs, report, run| $function(inputs, report, run), |_, _, _| Ok(())); };
    ($function:path, inputs, $start:path) => { $crate::export!(@body |inputs, _report, _run| $function(inputs), |i, p, r| $start(i, p, r)); };
    ($function:path, with_run, $start:path) => { $crate::export!(@body |inputs, report, run| $function(inputs, report, run), |i, p, r| $start(i, p, r)); };
    ($function:path, $start:path) => { $crate::export!(@body |inputs, report, _run| $function(inputs, report), |i, p, r| $start(i, p, r)); };
    (@body |$i:ident, $p:ident, $r:ident| $body:expr, |$si:pat_param, $sp:pat_param, $sr:pat_param| $start:expr) => {
        #[unsafe(no_mangle)]
        pub unsafe extern "C" fn tsugumi_card_invoke(raw: *const std::ffi::c_void) -> std::ffi::c_int {
            unsafe {
                if !$crate::__runs_card(raw) { return 0; }
                $crate::__invoke(raw, &mut |call| {
                    if $crate::__run_start(raw) {
                        let $si = $crate::Inputs::read(call)?;
                        let $sp = &$crate::Report::from_call(call);
                        let $sr = &$crate::Run::from_call(call);
                        return $start;
                    }
                    let $i = $crate::Inputs::read(call)?;
                    let $p = &$crate::Report::from_call(call);
                    let $r = &$crate::Run::from_call(call);
                    ($body)?.send(call)
                })
            }
        }
    };
}
''']
    open(os.path.join(out, 'src/lib.rs'), 'w').write('\n'.join(lines))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
