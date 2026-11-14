use tsugumi::{Inputs, Output, Report, Result};

fn run(_inputs: Inputs<'_>, report: &Report<'_>) -> Result<Output> {
    report.text("hello")?;
    Ok(Output::default())
}

tsugumi::export!(run);
