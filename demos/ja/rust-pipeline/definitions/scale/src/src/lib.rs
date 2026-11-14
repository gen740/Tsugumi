use tsugumi::{Report, Inputs, Output, Result};

fn run(inputs: Inputs<'_>, report: &Report<'_>) -> Result<Output> {
    let mut out = tsugumi::outputs::Out::default();
    out.value = inputs.number.value() * inputs.params.factor().unwrap_or(1);
    report.scalar(out.value as f64, "product")?;
    Ok(Output { out: Some(out) })
}

tsugumi::export!(run);
