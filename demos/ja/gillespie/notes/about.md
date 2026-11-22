## 化学反応のゆらぎ

細胞の中の分子は少なく、反応は 1 回ずつ確率的に起きます。Gillespie 法は、次の反応までの時間を指数分布から、どの反応かを速さの比から引いて、この過程を厳密にたどります。

$$\emptyset \xrightarrow{kV} X,\qquad X \xrightarrow{\gamma n} \emptyset,\qquad \frac{dn}{dt}=kV-\gamma n$$

この系の定常分布は平均 $N=kV/\gamma$ のポアソン分布なので

$$\mathrm{Var}[n]=N,\quad \frac{\mathrm{Var}[n]}{N}=1,\quad \mathrm{CV}=\frac{1}{\sqrt{N}}$$

です。反応が線形なので平均は率方程式と一致します。**binding** の $A+B\rightleftharpoons C$ は非線形で、小さな体積では平均そのものが率方程式から外れます。

**settings** の volume を 0.1 にすると分子は 2 個ほどになり、軌道は 0 にも落ちます。synthesis・degradation・runs も変えられます。速さの値は説明用で、特定の遺伝子の実測ではありません。
