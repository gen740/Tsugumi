use tsugumi::{Inputs, Output, Report, Result};

fn run(_inputs: Inputs<'_>, report: &Report<'_>) -> Result<Output> {
    report.text("hello")?;
    let mut out = tsugumi::outputs::Out::default();
    out.value = Some("hello".into());
    Ok(Output { out: Some(out) })
}

tsugumi::export!(run);
