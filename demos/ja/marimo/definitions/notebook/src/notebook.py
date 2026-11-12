"""測定値と、上流カードで計算した値を可視化します。"""
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
    mo.md(f"## ノートブックの結果\n\n変換後の値は **{result:g}** です。")
    return mo,


@app.cell
def figure(source, result):
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots()
    ax.bar(["測定値", "変換後"], [source, result])
    ax.set_title("測定値と変換後の値")
    fig
    return fig, ax, plt


if __name__ == "__main__":
    app.run()
