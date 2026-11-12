"""A normal marimo notebook; Tsugumi overrides the configuration cell."""
import marimo

app = marimo.App()


@app.cell
def configuration():
    source = 4.0
    factor = 2.0
    return source, factor


@app.cell
def calculation(source, factor):
    result = source * factor
    return result,


@app.cell
def explanation(result):
    import marimo as mo
    mo.md(f"## Notebook result\n\nThe selected result is **{result:g}**.")
    return mo,


@app.cell
def figure(source, result):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots()
    ax.bar(["Input", "Result"], [source, result])
    ax.set_title("One committed notebook invocation")
    fig
    return fig, ax, plt


if __name__ == "__main__":
    app.run()
