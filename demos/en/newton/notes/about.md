## Newton's method: the starting point decides where it goes

Search for roots of $p(z) = 0$ by the following iteration ($\lambda$ is the damping factor, usually 1).

$$z_{k+1} = z_k - \lambda\,\frac{p(z_k)}{p'(z_k)}$$

Near a simple root the error falls quadratically ($e_{k+1} \approx C e_k^2$). But which root it reaches is decided by the starting point, and the boundaries between them are fractal.

At a root of multiplicity $m$, it drops to first order, $e_{k+1} \approx (1-\lambda/m)\,e_k$, and returns to second order with $\lambda = m$. Near a critical point where $p' = 0$ it is thrown far away, and if it enters a cycle it never converges.

Set coeffs of **settings** to `[1, 0, -3, 2]` (a multiple root) or `[1, 0, -2, 2]` (a cycle), or damping to 0.5, and run the whole. The starting points of **orbits**, the zoom of **basins** and the offset of **trouble** can be rerun on their own Card.

A limit: the computation is in double precision, so the error at a multiple root levels off at about $\sqrt{\varepsilon} \approx 10^{-8}$.