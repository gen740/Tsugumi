# Vision and applications

**People and AI looking at the same processing, deepening the questions and growing the work together.**
Tsugumi was started for research, but the problem is common to much of the work done together with AI.

When you hand work to AI, it is hard to get your whole intention across in the first instruction.
Some assumptions you only notice once you see the result; sometimes you want to replace just one step in the middle.
"Keep this assumption." "Change only this part." "Don't touch this data."
What is needed is a foothold where such intentions are tied to concrete processing and shared by people and AI as they check.

**The flow of processing itself as the common language of the conversation.**
Convey the intention in a Card's description, show its boundary with typed inputs and outputs, and check the actual behaviour with the code and the results.
This page gathers the idea of extending this from research to education, engineering and everyday work.

## Turning research decisions into teaching material you can run

What people learning research really want to know is the accumulation of decisions behind a finished graph or paper.
Why this data was chosen, why this preprocessing was applied, why the comparison was made under these conditions.
Handed only the one script that survived at the end, they receive neither those intentions nor the trial and error that was rejected.

If the processing is split into Cards, with purposes and assumptions written in their descriptions and on sticky notes, the person teaching and the person learning can discuss while looking at the same structure.
The learner can change inputs and parameters and run them on the spot to see how the results change.
The teacher can follow concretely at which step the understanding of an assumption diverged.

For example, split the subtraction of background noise from observations and the fit of a model that follows into separate Cards.
Then replace only the method of estimating the background and compare how much the final conclusion is affected.
Through this back-and-forth, the material becomes something alive: not imitation of a procedure, but learning "why this analysis is done" by checking it with your own hands.

**Changing conditions and checking leads to learning the decisions of research.**
For that, the purpose of each step and the reason it was adopted are kept in Card descriptions and sticky notes, tied to steps that can be run. When learners bring the results they tried themselves, the next discussion can start from concrete processing too.

Related: [Usage](usage.md), [Cards and Workflows](features.md#2-cards-and-workflows), [Run history and artifacts](features.md#6-run-history-and-artifacts)

## Understanding and growing the tools you made with AI

Has a handy tool you made quickly by vibe coding turned into a black box that you are afraid to touch?
Or do you have AI build a similar tool from scratch, again and again, for uses that differ only slightly?

In a pipeline like "read files → clean them → aggregate → write a report", only the last step needs work to change the output format.
If the boundaries of the processing are clearly separated by Cards and the types of their ports, both the scope of the change you ask AI for and the scope a person must review can be kept to a minimum.
You can feed saved inputs into the rewritten Card, check its output, and then return to the whole Workflow.

In Tsugumi, a coherent piece of processing can also be handled as a child Workflow (a Card in a Card), and definitions can be imported from other projects and combined.
**Grow what you made once into the next use, checking as you go.**
Surveying, in one Workflow, what you change and how far the effect reaches, and recombining the parts you need to keep using them: that is Tsugumi's aim in engineering.

Related: [UI (the workbench)](features.md#8-ui-the-workbench), [Cards and Workflows](features.md#2-cards-and-workflows)

## Business automation and the data boundary

You want to automate routine work with AI. But to discuss it, is it all right to hand real customer information or unpublished internal documents to an outside AI?
Is telling the prompt "don't access this information" really enough?

The aim is to separate, as a mechanism, **the area where you build processing while consulting AI** from **the area where the processing runs on real confidential data**.
For example, have AI implement the aggregation logic using only dummy records and the type definitions of the inputs and outputs, and run it on production data in an isolated environment with outside communication cut off.
Limit what is fed back to the AI, too, to the statistics and error structure needed for checking, not raw data.
For that, it is not enough to visualise the flow of data; the system has to be able to enforce read permissions and the extent of outside transmission.

Tsugumi today has per-Card isolation with a Linux VM and bubblewrap, and the network cut off by default.
On the other hand, the source code, logs and input and output values the Agent reads through its tools are sent to the connected LLM provider.
**There is not yet a mechanism that automatically hides or masks confidential data from the Agent to prevent it from being sent.**
Building this data boundary into the design as types and permissions is the next important problem before extending to business automation in earnest.

How it works today: [Security and isolation](security.md), [What is sent to the model](security.md#what-is-sent-to-the-model)

## Handing work to the next person in a form they can run and check

"When the person who automated it moves on, nobody knows what it does inside."
"There is a handover document, but no way to check that it matches the script that is actually running."

For problems of work tied to one person, an approach that keeps the intention of the processing and an environment that can run it together is effective.

A Tsugumi project is an ordinary folder of the processing's definitions, source code and layout (`layout.json`), and can be managed with Git as it is.
Depending on the settings, the run history (`.tsugumi/runs.sqlite`) and artifacts travel with the project, so the person taking over has the material to follow past runs and try other conditions.

For example, when handing over a monthly aggregation, describe the assumptions about the input data, the rules for exceptions and the outputs to check, Card by Card.
The next person can discuss changes with AI while looking at that graph, try partial runs with the latest saved inputs, and then check the results through the whole Workflow.
**Make handing over work the starting point of the next person's trial and error.**
Being able to read the predecessor's intentions and check by changing conditions yourself is the foundation for understanding and continuing to improve.

Related: [Documents and saving](features.md#5-documents-and-saving), [Run history and artifacts](features.md#6-run-history-and-artifacts)

## Towards people and AI sharing the structure of the same work

What all these applications share is a relationship in which **people hold the intention, hand implementation and trials to AI, and can always return to the process to judge**.
Tsugumi realises that back-and-forth on tangible things: Cards, types, code and run records.

Research is the starting point.
**Towards a foundation where people and AI learn, try and improve together while looking at the structure of the same work.**
We will grow the ideas described here, one at a time, into real experience.

[Back to the README](../index.md#the-flow-of-processing-as-a-common-language-for-people-and-ai)
