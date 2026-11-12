use tsugumi::{Context, Input, Output, Result};

fn run(ctx: &Context<'_>, _input: Input<'_>) -> Result<Output> {
    ctx.display_text("hello")?;
    let mut out = tsugumi::outputs::Out::default();
    out.value = Some("hello".into());
    Ok(Output { out: Some(out) })
}

tsugumi::export!(run);
