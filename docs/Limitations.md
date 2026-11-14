# Before relying on a result

Use these checks when deciding whether a Workflow answers your question.

- Choose seeds and specify how changing external data, arrival order and retained worker state should affect the computation. [Reproducibility](Rationale.md#reproducibility) · [Worker state](CardGuide.md#worker-state)
- After experimenting with partial runs, run the whole Workflow before judging its result. [Run from a Card](Usage.md#run-from-a-card)
- Enable recording on the Cards whose input and output values you need to inspect, and retain File or Directory artifacts you need to keep. [Run history and artifacts](RunHistory.md)
- When comparing runs, inspect missing evidence and truncation markers before drawing a conclusion. [Compare two runs](RunHistory.md#compare-two-runs)
- Review the project author's permissions and any model or remote host receiving data. [Security boundaries](security.md#summary) · [Remote execution](RemoteExecution.md)
- For Julia registry packages, check the evaluator restriction before choosing dependencies. [Julia Cards](CardGuide.md#julia)

People judge scientific correctness. [Philosophy](Philosophy.md#a-clean-result-alone-cannot-be-checked) explains the responsibility behind these checks.
