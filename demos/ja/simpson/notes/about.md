## 集計と層別で逆転する関係

相関や比率は、データをまとめて見るか層に分けて見るかで符号まで変わることがあります（Simpson のパラドックス）。全体の相関は、層内の相関と層間の差の足し合わせです。

$$\operatorname{Cov}(X,Y) = \underbrace{\mathrm{E}[\operatorname{Cov}(X,Y \mid Z)]}_{\text{層内}} + \underbrace{\operatorname{Cov}(\mathrm{E}[X \mid Z],\, \mathrm{E}[Y \mid Z])}_{\text{層間}}$$

**上の枝** Palmer Penguins（Gorman, Williams & Fraser 2014、南極 Palmer 基地の実測。palmerpenguins パッケージ）。くちばしの長さと厚さの相関です。**correlate** の `stratify` を species / sex / island に変えて、その先を実行し直せます。**survey** は 3 つを並べて見せます。種では全層で逆転し、性別では逆転せず、島では Biscoe（アデリーとジェンツーが混ざる）だけ負のまま残ります。

**下の枝** 1973 年 Berkeley 大学院の出願（Bickel, Hammel & O'Connell 1975、R の `UCBAdmissions`）。全体では男性が通りやすく、学科で調整すると差はほぼ消えます。

**層別したほうが常に正しいわけではありません。** どちらを読むかは因果の問いで決まります。応募先の選択が性別の結果として生じるなら（媒介）、「性別の総合的な影響」を問うときに応募先で調整してはいけません。学科ごとの審査で差別があったかを問うなら、層別が答えに近づきます。ただしそれにも、学科内の応募者の違いという未測定の交絡が残ります。データだけではこの区別はつかず、値を読む前に問いを決める必要があります。