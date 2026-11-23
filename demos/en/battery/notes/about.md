## Scheduling a battery

With the stored energy $s_t$ as the state, dynamic programming decides the charging and discharging for each hour. The data is **all made up**.

$$V_t(s)=\min_{s'}\Big[\text{bill}_t(s'-s)+V_{t+1}(s')\Big],\quad V_T(s)=\begin{cases}0 & s=s_0\\ \infty & \text{otherwise}\end{cases}$$

With a one-way efficiency of $\sqrt{\eta}$, one round trip of buying at $p_{\rm low}$ and using at $p_{\rm high}$ pays only when

$$\eta\,p_{\rm high} > p_{\rm low}$$

At a flat tariff where buying and selling cost the same, $p_{\rm high}=p_{\rm low}$, so as long as $\eta<1$ not running it is optimal (the top of **tariffs**). If exports are cheaper, moving surplus solar from midday to the night pays by that much.

Try the tariff of **schedule**, and capacity_kwh, round_trip_efficiency and export_ratio of **settings**.

The stored energy can only move on its step (soc_step_kwh), so the answer is slightly worse than the true optimum. The **check** table shows by how much. Forecast errors, battery wear and the cost of the equipment are not included.
