## Counting cells

**The images in the upper row are synthetic data made from random numbers.** Their centres are known, so the counting can be scored.

Otsu's threshold is the value that best separates the variance of background and foreground. Only counting connected regions makes touching cells one.

Taking the maxima of the distance transform $d(p)$ (the distance from a foreground point $p$ to the background) as cell centres and applying watershed to $-d$ separates them.

$$\mathrm{precision} = \frac{TP}{TP + FP},\qquad \mathrm{recall} = \frac{TP}{TP + FN}$$

Change cells and noise of **generate** and run again, or look at the table of **sweep**, to see from where counting fails. The real image in the lower row has no ground truth, so check its outlines by eye.

The synthetic cells are nearly round, easier than real cells. Scores here do not guarantee scores on real images.