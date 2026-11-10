# What to build so that we are not used by AI

"Research should use AI more." I have thought so for a long time.
At the same time, I have felt a strong unease about handing research over to AI as it is.
Isn't it people who should find the scientific meaning and take responsibility for the conclusions drawn?
Thinking between these two feelings is what led me to start building Tsugumi.

## Moving research forward, and giving it meaning

What I want is not research with AI as the protagonist, but research in which people ask the questions and the power of AI moves the trial and error forward.
Form a hypothesis. Test it. Change the conditions and try again.
It is the researcher who gives that process meaning, takes responsibility for the results and decides which question to open up next.

I would like to call research done this way **AI-driven science**.
If it became the norm, research would be more enjoyable and more creative than it is now.

## "Be the one who uses AI" is not easy

People often say: "Be the one who uses AI. Don't let AI use you." That is entirely right, but holding to it takes an enormous amount of effort.
It is the researcher who has to read the code the AI wrote, check its implicit assumptions and keep up with one long report after another.

Can the speed at which people understand the process and make decisions keep up with the speed at which AI runs its trials?

## After handing over, can people come back to judging?

Research on automated driving says something important about this question.
Driving automation is divided into six levels, from Level 0 to Level 5, and the roles of person and system change at each level ([NHTSA's classification](https://www.nhtsa.gov/vehicle-safety/automated-vehicle-safety)).

The hardest is **Level 3**. Most of the time the system drives, and a person takes over only when asked to.
It has been hypothesised that the load on situation awareness at the moment of takeover is highest at Level 3 ([Li et al., §2.2 and Figure 2.2](https://rosap.ntl.bts.gov/view/dot/73650/dot_73650_DS1.pdf#page=14)).

A person who has been away from driving for a while, suddenly told in an emergency "from here on, you decide", cannot immediately grasp what is going on.

Simulator experiments, too, found little difference in the time until drivers were physically ready to operate, and argued that what decided whether a takeover succeeded was mainly the cognitive process ([Zeeb, Buchner & Schrauf, 2015](https://pubmed.ncbi.nlm.nih.gov/25794922/)).
**Being able to hold the steering wheel is not the same as understanding the situation well enough to drive.**

I feel that using AI in research has exactly the same structure.
Even if we decide that "a person checks the final conclusion", how much does that check mean if we have lost sight of the process that led there?

## A clean result alone cannot be checked

What if only the samples that showed a strong correlation had been conveniently chosen? What if the conditions that did not work had quietly dropped out of the report?
A beautiful final graph and a smooth explanation give you no way to notice that distortion.

Check how the samples were chosen. Change the random seed. Compare with another model.
That unglamorous checking is the researcher's job, and it is also the soil from which the next new question grows.

That is why I want the process of trial and error, and the steps of checking, kept in a form people can return to and check at any time.
Each step of processing split into a size you can see through, so you can follow what went in, what was done and what came out.
The conditions of a step you were curious about changed, and run again by your own hand.
Connecting AI and researchers in this way is where Tsugumi starts.

## Where it started

This section condenses an essay the author wrote in Japanese, deliberately without any AI (no Claude, ChatGPT or Gemini, not even for structure or proofreading), on the night of 6 November 2026. The original is kept in the [hackathon snapshot](https://github.com/gen740/tsugumi-hackathon/blob/main/docs/Origin.md); what follows is an AI translation of its substance.

The first specification of Tsugumi had been in the making long before the hackathon. The problem it addresses, and the way it addresses it, come from years of doing research while working with AI. How can research use AI actively and still remain something you enjoy as your own? AI is wonderful: it now knows more than people do, and its ability to turn knowledge into form has grown enormously. But is it really fun if AI does everything? What is it that only people can do?

The author studies astrophysics, a field whose analysis is not simple. Observations carry noise peculiar to space and limits set by the detectors themselves, and the software that analyses them, NASA's open-source [HEASoft](https://heasarc.gsfc.nasa.gov/docs/software/lheasoft/), covers many detectors and is long and hard to use.

Yet space should be close to everyone. Article I of the Outer Space Treaty says that the exploration and use of outer space "shall be carried out for the benefit and in the interests of all countries, irrespective of their degree of economic or scientific development, and shall be the province of all mankind." And space is, in fact, within reach: the data is public on the internet and can be downloaded with one command. With that alone, you can touch messages from deep space observed by the technology people have built up. The author wanted more people to know this, and to be able to do research more easily.

Thinking about how to provide reproducible environments for astrophysics led to Nix, and to [gen740/heasoft](https://github.com/gen740/heasoft), a Nix packaging of the analysis software that the author publishes and uses. Still, the barriers remained, and the author could not yet bring it all into a form within everyone's reach. The architecture, the software structure and the UI had been in mind for a long time, but building software that satisfied all of them was too large for one person. Before the hackathon there were countless prototypes; some got far enough to seem usable, but there was always one step missing. The AI of a few months earlier lacked the power to hold the whole together and build it out under one consistent philosophy.

Claude gave that idea a form. A design refined over a long time took shape in three days. When the author pasted in the specification, Claude's first words were: "The whole specification is large (months of work). How far should we aim in this session?" And then it ran all the way. The author had been prepared for this to be one more attempt among many; instead, they came out of it truly proud of what they made.

The essay ends with a call to everyone who shares the concern:

> "What can we do so that human intellectual and creative activity is not let go of, and remains ours?"

## The name

**Tsugumi** carries several meanings, all connected to why the tool was made.

First, *tsugu*, *tsumu*, *kumu*: to carry on, to stack, to assemble. Carry small steps of processing on from one to the next, stack trial and error up one at a time, and assemble them into the experiment or analysis that tests your own question. Try, check, and pass it on to the next step: that is what research is.

Then, from *tsugumi* to *tsugumu*, and by swapping its sounds (a metathesis), to *tsumugu*, to spin. The tool should help you spin your own research out of fragments of trial and error. The temari (a traditional thread ball) and the thread that the thrush holds in its beak, in the icon and on screen, come from this: a single thread drawn from the temari becomes the wire that connects Card to Card.

The other origin is the bird, the dusky thrush (*tsugumi* in Japanese). It comes to Japan as a winter bird, about 24 cm long, and is known for hopping a few steps on the ground, stopping, and standing up straight to look around. It does not dash through; it moves, stops, checks where it is, and moves on. That is also how research with AI should move.

One account says the bird got its name because it does not sing in Japan during winter, as if keeping its beak shut, *tsugumu* ([Suntory, "Encyclopedia of Japanese birds"](https://www.suntory.co.jp/eco/birds/encyclopedia/detail/1359.html)). However eloquently AI reports, Tsugumi should not rely on explanation in words alone, but let the process itself, what was actually tried and checked, speak. By following each step, working on it and running it again, researchers can build up their own conviction. In an age that tells results with ever more words, the tool quietly offers the accumulated attempts. That is the bearing of the thrush with its beak shut.

## How it was built

This project is itself an experiment. A runtime and an application of this size were built in the single week of a Claude hackathon, in a back-and-forth where a person wrote intentions and specifications in natural language and AI gave them form.

- **Prepared before the hackathon:** the only code of the author's own that existed beforehand is [hosq](https://github.com/CLI20-dev/hosq), a C++ JSON parser, used for Tsugumi's documents and settings; its version is pinned in [flake.lock](../flake.lock). The first specification prompt, which worked out Cards and Workflows, the runtime, the communication contract and the boundary between UI and Agent in advance, is kept in full in the [first commit's instruction.md](https://github.com/gen740/Tsugumi/blob/98f90945ed92477e6e65ff48f1f410271edaaf96/instruction.md).
- **Made during the hackathon:** everything else specific to Tsugumi: the document model and scheduler (`core/`, `protocol/`), the Card SDK and the demos (`sdk/`, `demos/`), the macOS app and its VM (`macOS/`), the Web UI (`web/`), the Agent (`agent/`), the `tsugumi` command and the Claude Code plugin (`cli/`, `plugins/`), and the documentation site (`docs/site/`). A person carried the specification, the decisions and the review; AI agents implemented and fixed. Libraries and tools from elsewhere (FlatBuffers, Nix, SQLite, libgit2, SwiftUI, Dawn, Tree-sitter, React and the others listed in [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)) are their authors' work, and code generated from Tsugumi's schemas is not counted as written by hand.
- **The record:** the state at submission is the archived [gen740/tsugumi-hackathon](https://github.com/gen740/tsugumi-hackathon) (tag `hackathon-submission`), with its documents as they were then. The day-by-day record of building it with Claude is [Tsugumi Chronicle](https://gen740.github.io/Tsugumi-chronicle/). The Git history of this repository runs on from there; commit dates are when something was recorded, not necessarily when it was made.

Once you can describe concretely what you want to build, the limit of what one person can make widens astonishingly. But then what is needed, as urgently as speed of building, is **a tool for people to understand, judge and keep working on what was built**.

Tsugumi is my own answer to the question of how to connect people and AI.
The question reaches beyond research to education, software development and everyday work ([Vision and applications](Vision.md)).

**To do research and work without being used by AI, what tools will you build?**

## The author

A first-year PhD student at the University of Tokyo, researching AI for astrophysics. Tsugumi grew out of what they had been thinking about in that research.

[Back to the README](../README.md#the-flow-of-processing-as-a-common-language-for-people-and-ai)
