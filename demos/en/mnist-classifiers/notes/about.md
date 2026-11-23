## Telling handwritten digits apart seven ways

The same MNIST subset goes to seven classifiers, each trained at the same time in its own Nix environment, and they are compared at the end on the same test images.

$$\mathrm{accuracy} = \frac{1}{N}\sum_{i=1}^{N} \mathbf{1}[\hat y_i = y_i], \qquad \mathrm{SE} \approx \sqrt{\frac{p(1-p)}{N}}$$

Read differences in accuracy against the error SE set by the number of test images $N$. Times are single-thread CPU times and vary by machine.

**Try**: change a parameter of a classifier Card (k of knn, max_depth of forest, epochs of cnn and so on) and run just that Card; the other branches do not move, and only that Card's display changes. To compare again, run from that Card onwards. compare compares the new result with the saved inputs of the other branches, so the accuracies of the branches you did not change stay the same. How many Cards run at once is "Parallel runs" in the settings (4 by default).

The default counts are not the whole set (10000 training, 2000 test). To compare with published figures, increase the counts in prepare.