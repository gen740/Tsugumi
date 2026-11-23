## A relationship that reverses between the aggregate and the strata

A correlation or a ratio can change even its sign depending on whether the data is seen together or split into strata (Simpson's paradox). The overall correlation is the sum of the correlation within strata and the differences between them.

$$\operatorname{Cov}(X,Y) = \underbrace{\mathrm{E}[\operatorname{Cov}(X,Y \mid Z)]}_{\text{within strata}} + \underbrace{\operatorname{Cov}(\mathrm{E}[X \mid Z],\, \mathrm{E}[Y \mid Z])}_{\text{between strata}}$$

**Upper branch**: Palmer Penguins (Gorman, Williams & Fraser 2014, measured at Palmer Station, Antarctica; the palmerpenguins package). The correlation of bill length and depth. Change `stratify` of **correlate** to species / sex / island and rerun from there. **survey** shows the three side by side. By species it reverses in every stratum, by sex it does not, and by island only Biscoe (where Adélie and Gentoo mix) stays negative.

**Lower branch**: applications to Berkeley's graduate school in 1973 (Bickel, Hammel & O'Connell 1975; R's `UCBAdmissions`). Overall, men were admitted more readily; adjusted for department, the difference almost disappears.

**Stratifying is not always right.** Which to read is decided by the causal question. If the choice of department arises as a consequence of sex (mediation), you must not adjust for department when asking about "the total effect of sex". If the question is whether each department's review discriminated, stratifying gets closer to the answer. Even then, an unmeasured confounder remains: differences among applicants within a department. The data alone cannot tell these apart; the question has to be decided before reading the values.