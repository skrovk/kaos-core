# Graph theory ideas for KaOS

The most useful concepts concern dependency choices, failure tolerance and
placement. Keep three models separate: the **application directed multigraph**
(nodes and service-labelled channels), the **directed transport graph** (hosts
and communication links), and the **dependency expressions** controlling startup.
Their distinctions follow from the [architecture](architecture.md); applications
below are proposed analyses, not claims of existing algorithms.

**1. Hypergraphs, minimal support sets and hitting sets.** A hyperedge can contain
more than two vertices, making it useful for representing requirements that must
hold together. For KaOS's `P = (A OR B) AND C`, the minimal channel sets satisfying
P are `{A,C}` and `{B,C}`. Treat these as hyperedges. A failure set that intersects
every support set destroys all alternatives: the minimal such sets are `{C}` and
`{A,B}`. Here “minimal” means no member can be removed; only `{C}` has minimum
cardinality. This exposes indispensable channels and suggests targeted failure
tests. In [the predicate interface](../graph_core/include/kaos_predicate.h), each
channel requires both endpoint roles; a half-prepared channel cannot support P.
Learn from NASA's [Fault Tree Handbook, chapter 3](https://s3vi.ndc.nasa.gov/ssri-kb/static/resources/Fault%20Tree%20Handbook_NASA.pdf)
on success/failure sets, then the research paper on
[hypergraph transversals](https://arxiv.org/abs/cs/0204009) for the mathematical connection.

**2. Strongly connected components and condensation.** An SCC is a maximal group
of vertices mutually reachable along directed paths. Collapsing each SCC produces
a directed acyclic graph, giving a compact overview of feedback regions and
one-way stages. This could make large application graphs easier to visualize and
identify groups worth testing together. It does **not** establish deadlock:
[node startup](../graph_core/kaos_node.c) depends on prepared/active channels,
not simply on predecessor execution. A real wait-for graph needs actual blocking
conditions. Learn from Sedgewick and Wayne's
[Directed Graphs](https://algs4.cs.princeton.edu/42digraph/), especially strong
connectivity and topological order.

**3. Cuts and disjoint paths.** A directed s–t edge cut removes every path from
s to t. With unit edge capacities, its minimum size equals the maximum number
of edge-disjoint paths. This quantifies structural redundancy and suggests small
partition scenarios for experiments. Use host failure domains too: two application
paths can share a physical host and fail together. These connectivity cuts differ
from predicate failure sets above. KaOS's [transport model](architecture.md)
has independently impaired directions; it does not automatically forward traffic
through intermediate hosts. Application paths require suitable guest behavior.
Learn from [Maximum Flow](https://algs4.cs.princeton.edu/64maxflow/), including its
disjoint-path exercises.

**4. Temporal graphs and time-respecting paths.** A static graph can claim a path
exists even when its edges are usable in the wrong order. Temporal graphs attach
times or intervals to edges. For a forwarding application, A→B available only
after B→C closes provides no time-respecting A→C path. This lens could guide relay
experiments involving activation, partitions and deadlines; preserve actual
buffering and expiry rules. [KaOS outcomes](architecture.md#evidence-and-time)
record historical success, so they cannot serve as a live connectivity snapshot.
Learn from Holme and Saramäki's [Temporal Networks](https://arxiv.org/abs/1108.1780),
especially temporal paths and reachability.

**5. Weighted graph partitioning for placement.** Assign compute/memory weights
to application vertices and expected traffic weights to channels, then seek host
groups with affordable resource loads and little cross-host traffic. This could
inform an orchestrator placement planner. KaOS also needs hard constraints for
[nodes, endpoint roles and records](../graph_core/include/kaos_limits.h), including
[resources awaiting cleanup](../graph_core/kaos_admission.c). Keeping redundant
branches on separate hosts can conflict with minimizing communication. Learn from
the [METIS manual, sections 3.1–3.5](https://karypis.github.io/glaros/files/sw/metis/manual.pdf).
Its undirected partitioning model is a starting point; directional traffic and
KaOS admission feasibility require additional modelling.
