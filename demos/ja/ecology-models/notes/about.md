## 生態系のモデル比較

ハドソン湾会社の毛皮の記録（**実データ**、1900〜1920 年、出典: [stan-dev/example-models](https://github.com/stan-dev/example-models/blob/master/knitr/lotka-volterra/hudson-bay-lynx-hare.csv)、Howard 2009 の講義資料から）に、3 つの捕食・被食モデルを当てはめます。

$$\text{LV: } \dot H=aH-bHL,\ \ \dot L=cHL-dL$$
$$\text{被食者ロジスティック: } aH\to aH(1-H/K)$$
$$\text{ホリング II 型: } bH\to \frac{bH}{1+bhH}$$

誤差は対数の二乗和で、同じ分散の正規ノイズを仮定して $\mathrm{AIC}=n\log(\mathrm{SSE}/n)+2k$ で比べます。

**AIC が良くても、その仕組みが正しいとは言えません**。AIC は手元の式の中での相対的な順位を示すだけで、残差の自己相関も無視しています。また**毛皮の数は個体数ではなく**、罠の数や毛皮の値段、捕らえやすさ（飢えた個体は罠にかかりやすい）に左右される代理の値です。

**forecast** の fit_until で残す年を、**fit** の restarts で初期値の数を変えて試せます。
