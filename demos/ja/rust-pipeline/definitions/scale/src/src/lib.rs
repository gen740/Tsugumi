use tsugumi::{Inputs, Markdown, Output, PARAMS, Result, send_value};

fn run(inputs: Inputs) -> Result<Output> {
    let mut out = tsugumi::outputs::Out::default();
    out.value = inputs.number.value() * PARAMS.factor as u64;
    send_value(Markdown(&format!("**Product**: {}", out.value)))?;
    Ok(Output { out: Some(out) })
}

tsugumi::export!(run);
