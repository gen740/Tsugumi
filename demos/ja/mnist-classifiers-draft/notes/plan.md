MNIST の分類器くらべ
load と prepare は実装済み。7 つの分類器は互いに依存しないので、Agent は手分けして同時に書ける（delegate）。そろったら compare を書く