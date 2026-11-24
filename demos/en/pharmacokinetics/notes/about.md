## Pharmacokinetics: the dosing interval and how concentration moves

**This is a model for teaching with a made-up drug, "Tsugumicin". Its values are not those of any real drug, and it is neither medical advice nor dosing guidance.** The therapeutic range is invented too.

In a one-compartment oral model, the amount of drug in the gut $A$ and the blood concentration $C$ follow

$$\frac{dA}{dt} = -k_a A,\qquad \frac{dC}{dt} = \frac{k_a A}{V} - k_e C,\qquad k_e = \frac{\ln 2}{t_{1/2}}$$

Each dose adds $F D$ to $A$, and since it is linear, the responses to each dose add up. With $k_a \gg k_e$, the accumulation ratio of the troughs is $R \approx 1 / (1 - e^{-k_e \tau})$. With the same daily dose and half-life the mean concentration is the same, and shortening the interval narrows the gap between peak and trough.

Change doses_mg, intervals_h and half_life_h of **simulate** and run the whole. missed_at_h and missed_count of **missed** can be rerun on their own Card.

Limits: the body is treated as one box, absorption and elimination are first order, and there is no variation between individuals.