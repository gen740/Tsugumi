---
title: fit
description: Fit three models
---

Fits Lotka–Volterra, logistic prey and Holling type II with Nelder–Mead so that the squared error of the logs is smallest, solving them with RK4 (in log space, from restarts starting points). The initial values H0 and L0 are among the parameters. Finds the error, AIC, AICc and BIC, and overlays the simulations on the records.
