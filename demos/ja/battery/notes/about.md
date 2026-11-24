## 蓄電池の最適運用

蓄電量 $s_t$ を状態に、1 時間ごとの充放電を動的計画法で決めます。データは**すべて架空**です。

$$V_t(s)=\min_{s'}\Big[\text{bill}_t(s'-s)+V_{t+1}(s')\Big],\quad V_T(s)=\begin{cases}0 & s=s_0\\ \infty & \text{それ以外}\end{cases}$$

片道の効率を $\sqrt{\eta}$ とすると、$p_{\rm 安}$ で買って $p_{\rm 高}$ で使う 1 往復が得になるのは

$$\eta\,p_{\rm 高} > p_{\rm 安}$$

のときだけです。買値と売値が同じ定額料金では $p_{\rm 高}=p_{\rm 安}$ なので、$\eta<1$ である限り動かさないのが最適です（**tariffs** の一番上）。売電が安ければ、昼の余った太陽光を夜に回す分だけ得になります。

**schedule** の tariff、**market** の capacity_kwh・round_trip_efficiency・export_ratio を変えて試せます。

蓄電量は刻み（soc_step_kwh）の上でしか動かせないので、答えは本当の最適より少しだけ悪くなります。その差は **check** の表で分かります。予報の誤差、電池の劣化、機器の代金は入れていません。
