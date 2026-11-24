## Maximum flow and minimum cut

In a directed graph with capacities, the most that can flow from $s$ to $t$ equals the smallest total capacity of edges that separate $s$ from $t$.

$$\max_f |f| = \min_{(S,T)} \sum_{u \in S,\, v \in T} c(u, v)$$

However much you widen an edge that is not in the cut, that cut's capacity stays the same, so the maximum flow does not increase. Widening an edge of the cut gains nothing if another minimum cut remains, and when it does gain, it stops where the next cut gets in the way.

The network is a made-up one, made from the seed.

Change seed of **network** and run the whole. edge and raise_by of **whatif**, and raise_by of **sweep**, can be rerun on their own Card.