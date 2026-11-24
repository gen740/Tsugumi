## An inverse problem in heat conduction

The problem of finding a rod's initial temperature from its temperature at a later time. The data is **made up**, with random noise added.

$$u_t=\alpha u_{xx},\quad u(x,T)=\sum_k e^{-\alpha\lambda_k T}\langle u_0,\phi_k\rangle\phi_k$$

The finer a mode, the faster it vanishes as $e^{-\alpha\lambda_k T}$, so solving backwards amplifies the noise by the same factor (**naive**). Tikhonov regularisation damps the components of small singular values.

$$\min_u\ \|Au-d\|^2+\lambda^2\|u\|^2,\quad \hat u=\sum_k\frac{\sigma_k}{\sigma_k^2+\lambda^2}(u_k^{\mathsf T}d)\,v_k$$

$\lambda$ is chosen by the discrepancy principle $\|A\hat u-d\|\approx\sigma_{\rm noise}\sqrt{n}$ or at the corner of the L-curve (**tikhonov**). The "closest $\lambda$", which needs the truth, is only for comparison.

Try final_time and noise of **forward**, and lambda of **reconstruct**. **horizon** shows the details lost by measuring late.

The size of the noise is taken as known. On this grid the vanished modes cannot be recovered, and the recovery is "one solution consistent with the measurements".
