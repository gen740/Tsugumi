use tsugumi::{Context, Input, Output, Result};

fn run(ctx: &Context<'_>, input: Input<'_>) -> Result<Output> {
    let mut out = tsugumi::outputs::Out::default();
    out.value = input.number.value() * ctx.params()?.factor().unwrap_or(1);
    ctx.display_scalar(out.value as f64, "product")?;
    Ok(Output { out: Some(out) })
}

tsugumi::export!(run);
