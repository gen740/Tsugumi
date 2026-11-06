"""Waits for every branch, checks they scored the same test digits, and ranks them."""
import numpy as np

# All seven branches send the same table, so one generated class reads them all.
from inputs.knn.mnist.Result import Result
from inputs.data.mnist.SubsetFile import SubsetFile
import outputs.comparison.mnist.Comparison as out
from tsugumi import Failure

MODELS = ["knn", "logistic", "svm", "forest", "boosting", "mlp", "cnn"]


def main(ctx):
    data = np.load(ctx.input("data", SubsetFile).Value().decode())
    labels = data["test_labels"]
    results = [ctx.input(m, Result) for m in MODELS]
    predictions = np.stack([r.PredictionsAsNumpy() for r in results])

    # The checks: same test digits, and each reported accuracy recomputed here.
    for m, r, pred in zip(MODELS, results, predictions):
        if r.Digest().decode() != str(data["digest"]):
            raise Failure("output_invalid", f"{m} was scored on other test digits: run it again")
        if abs(np.mean(pred == labels) - r.Accuracy()) > 1e-12:
            raise Failure("output_invalid", f"{m} reports {r.Accuracy()}, its predictions give {np.mean(pred == labels)}")

    accuracy = np.array([r.Accuracy() for r in results])
    fit = np.array([r.FitSeconds() for r in results])
    predict = np.array([r.PredictSeconds() for r in results])
    settings = [r.Settings().decode() for r in results]
    n = len(labels)
    order = np.argsort(-accuracy, kind="stable")

    rows = [f"| {rank + 1} | **{MODELS[i]}** | {accuracy[i]:.4f} | {fit[i]:.2f} | {predict[i]:.3f} | {settings[i]} |"
            for rank, i in enumerate(order)]
    best, second = order[0], order[1]
    # Two models scored on the same digits: what separates them is the digits only one gets right.
    only_best = int(np.sum((predictions[best] == labels) & (predictions[second] != labels)))
    only_second = int(np.sum((predictions[best] != labels) & (predictions[second] == labels)))
    disagree = only_best + only_second
    margin = accuracy[best] - accuracy[second]
    noise = np.sqrt(disagree) / n
    verdict = (f"差 {margin:.4f} は、この 2 つの判定が割れた {disagree} 枚から見込まれる揺らぎ（約 {noise:.4f}）の"
               + ("2 倍以上あり、この枚数でも差があると言えます。" if margin >= 2 * noise else
                  "2 倍に届かず、この枚数では順位を決められません。"))
    se = np.sqrt(accuracy[best] * (1 - accuracy[best]) / n)
    fastest_fit = np.argmin(fit)
    fastest_predict = np.argmin(predict)
    ctx.display_markdown(
        f"**テスト {n} 枚での比較**（全員同じ画像。正解率は予測から計算し直して確認済み）\n\n"
        "| 順位 | モデル | 正解率 | 学習 (秒) | 予測 (秒) | 設定 |\n| ---: | --- | ---: | ---: | ---: | --- |\n"
        + "\n".join(rows)
        + f"\n\n最も正解率が高いのは **{MODELS[best]}**（{accuracy[best]:.4f}、標準誤差 およそ {se:.4f}）で、"
          f"次が **{MODELS[second]}**（{accuracy[second]:.4f}）です。{verdict}\n\n"
          f"学習が最も短いのは **{MODELS[fastest_fit]}**（{fit[fastest_fit]:.2f} 秒）、"
          f"予測が最も短いのは **{MODELS[fastest_predict]}**（{predict[fastest_predict]:.3f} 秒）です。"
          "時間は 1 スレッドの CPU 時間で、機種によって変わります。"
    )

    b = ctx.builder()
    names = [b.CreateString(m) for m in MODELS]
    out.StartModelsVector(b, len(names))
    for name in reversed(names):
        b.PrependUOffsetTRelative(name)
    models_offset = b.EndVector()
    accuracy_offset = b.CreateNumpyVector(accuracy)
    fit_offset = b.CreateNumpyVector(fit)
    predict_offset = b.CreateNumpyVector(predict)
    labels_offset = b.CreateNumpyVector(labels)
    predictions_offset = b.CreateNumpyVector(predictions.ravel())
    images = np.round(data["test_images"].ravel() * 255).astype(np.uint8)
    images_offset = b.CreateNumpyVector(images)
    out.Start(b)
    out.AddModels(b, models_offset)
    out.AddAccuracy(b, accuracy_offset)
    out.AddFitSeconds(b, fit_offset)
    out.AddPredictSeconds(b, predict_offset)
    out.AddTestLabels(b, labels_offset)
    out.AddPredictions(b, predictions_offset)
    out.AddTestImages(b, images_offset)
    ctx.output("comparison", b, out.End(b))
