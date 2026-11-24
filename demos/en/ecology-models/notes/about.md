## Comparing ecological models

Fits three predator–prey models to the Hudson's Bay Company's fur records (**real data**, 1900–1920, source: [stan-dev/example-models](https://github.com/stan-dev/example-models/blob/master/knitr/lotka-volterra/hudson-bay-lynx-hare.csv), from Howard 2009's lecture notes).

$$\text{LV: } \dot H=aH-bHL,\ \ \dot L=cHL-dL$$
$$\text{Logistic prey: } aH\to aH(1-H/K)$$
$$\text{Holling type II: } bH\to \frac{bH}{1+bhH}$$

The error is the sum of squares of the logs; assuming normal noise of equal variance, they are compared by $\mathrm{AIC}=n\log(\mathrm{SSE}/n)+2k$.

**A good AIC does not mean the mechanism is right**. AIC only ranks the equations at hand relative to each other, and ignores the autocorrelation of the residuals. And **pelt counts are not population counts**: they are a proxy swayed by the number of traps, the price of pelts and how easy the animals are to catch (starving animals walk into traps more readily).

Try changing the years held out with fit_until of **forecast**, and the number of starting points with restarts of **fit**.
