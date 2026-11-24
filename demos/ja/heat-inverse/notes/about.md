## 熱伝導の逆問題

あとの時刻の温度から、棒の初期温度を当てる問題です。データは**架空**で、乱数のノイズをのせています。

$$u_t=\alpha u_{xx},\quad u(x,T)=\sum_k e^{-\alpha\lambda_k T}\langle u_0,\phi_k\rangle\phi_k$$

細かいモードほど $e^{-\alpha\lambda_k T}$ で速く消えるので、逆に解くとノイズが同じ倍率で増えます（**naive**）。Tikhonov 正則化は小さな特異値の成分を弱めます。

$$\min_u\ \|Au-d\|^2+\lambda^2\|u\|^2,\quad \hat u=\sum_k\frac{\sigma_k}{\sigma_k^2+\lambda^2}(u_k^{\mathsf T}d)\,v_k$$

$\lambda$ は食い違い原理 $\|A\hat u-d\|\approx\sigma_{\rm noise}\sqrt{n}$ か L 曲線の角で選びます（**tikhonov**）。真の値が分かっている「最も近い $\lambda$」は比較のためだけのものです。

**forward** の final_time・noise、**reconstruct** の lambda を変えて試せます。**horizon** は遅く測ると失われる細部を示します。

ノイズの大きさは既知としています。この格子では消えたモードは取り戻せず、復元は「測定と矛盾しない解のひとつ」です。
