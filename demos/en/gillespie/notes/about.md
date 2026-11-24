## Fluctuations in chemical reactions

Molecules in a cell are few, and reactions happen one at a time, at random. Gillespie's method follows this process exactly, drawing the time to the next reaction from an exponential distribution and which reaction it is from the ratio of the rates.

$$\emptyset \xrightarrow{kV} X,\qquad X \xrightarrow{\gamma n} \emptyset,\qquad \frac{dn}{dt}=kV-\gamma n$$

The stationary distribution of this system is Poisson with mean $N=kV/\gamma$, so

$$\mathrm{Var}[n]=N,\quad \frac{\mathrm{Var}[n]}{N}=1,\quad \mathrm{CV}=\frac{1}{\sqrt{N}}$$

The reactions are linear, so the mean agrees with the rate equation. $A+B\rightleftharpoons C$ in **binding** is non-linear, and in small volumes the mean itself departs from the rate equation.

Set volume of **trajectories** and **stationary** to 0.1 and there are only about two molecules; trajectories drop to 0 too. synthesis, degradation and runs can be changed as well. The rates are for illustration, not measurements of any particular gene.
