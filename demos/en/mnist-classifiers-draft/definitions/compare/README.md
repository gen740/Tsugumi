---
description: Compare the seven classifiers side by side
---

Takes data and the seven classifiers' result, waiting until all have arrived. The seven inputs have the same type (mnist.Result); read each through its named input port. Show a ranking of model name, accuracy, training time, prediction time and settings as Markdown, and a matplotlib scatter plot of accuracy against training time (log horizontal axis). On the gap between first and second, compare it with the fluctuation expected from the number m of images on which the two disagree, √m / number of test images, and say in words that at twice that or more there is a difference, and short of it the order cannot be decided with this many images. Assert that all seven digests equal data's digest, that each reported accuracy agrees to 1e-12 with the value recomputed from predictions and test_labels, and that the highest of the seven accuracies is higher than the lowest. Use numpy and matplotlib.

Planned inputs:
- data: prepare's subset (the test labels and the digest)
- knn: knn's result
- logistic: logistic's result
- svm: svm's result
- forest: forest's result
- boosting: boosting's result
- mlp: mlp's result
- cnn: cnn's result
