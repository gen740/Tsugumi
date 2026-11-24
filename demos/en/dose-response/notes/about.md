## Dose–response curve (made-up data)

**This plate is synthetic data made from random numbers.** Compounds A and B are made up, and this is not medical advice.

The response is modelled as a four-parameter logistic (Hill curve) of the concentration $x$:

$$y = b + \frac{t - b}{1 + (\mathrm{EC}_{50}/x)^{h}}$$

**fit** fits it by least squares and **bootstrap** adds 95% intervals. The true values are known, so you can check whether they were recovered.

Change noise_sd, concentrations and top_um of **generate** and run again. With top_um at 1, B's curve never levels off and its interval opens up (**range** shows the same while changing the highest concentration).

The residual bootstrap assumes that the noise does not depend on the concentration and that the 4PL shape is right.