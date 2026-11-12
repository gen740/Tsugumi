"""Visualize a measurement and the value computed by the upstream Card."""
import marimo

app = marimo.App()


@app.cell
def configuration():
    source = 4.0
    result = 8.0
    return source, result


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
    ax.set_title("Measurement and scaled value")
    fig
    return fig, ax, plt


if __name__ == "__main__":
    app.run()
