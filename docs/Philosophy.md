# What to build so that we are not used by AI

"Research should use AI more." I have thought so for a long time.
At the same time, I have felt a strong unease about handing research over to AI as it is.
Isn't it people who should find the scientific meaning and take responsibility for the conclusions drawn?
Tsugumi connects these two concerns.

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

## A laboratory notebook, and who decides what is kept

The closest thing I know is the laboratory notebook.
At the bench you write down what you used, what you did and what came out, as it happens. The notebook is not the clean account written afterwards.
Computation lost that habit somewhere. Tsugumi is meant to be that notebook for analysis: one you can open at any step, and run again.

A notebook also draws a line, and I think this line matters most.

Some things a machine can say are the same.
Code and the environment it runs in are fixed by their definition, and are the same wherever and whenever they are built.
Those the machine may build once, share and reuse without asking anyone.

Results are different. What a result means, and whether it is worth keeping, is not something a machine can say.
So Tsugumi keeps the record of what ran, and the inputs of the last run so that you can try again. Beyond that, it keeps only the values and files a person chose to keep.
It may suggest clearing old runs. It should not decide on its own what to keep, what to reuse in place of a new run, or what to throw away.
Data you want shared the way code is shared, you put into a definition yourself, and by doing so you are the one who says it is fixed.

Caching every result and reusing it automatically would be more convenient.
But a tool that quietly decides which result stands in for which has taken over a judgement that belongs to the researcher.
Where convenience and that judgement pull apart, Tsugumi chooses the judgement.

## The name

**Tsugumi** carries several meanings, all connected to why the tool was made.

First, *tsugu*, *tsumu*, *kumu*: to carry on, to stack, to assemble. Carry small steps of processing on from one to the next, stack trial and error up one at a time, and assemble them into the experiment or analysis that tests your own question. Try, check, and pass it on to the next step: that is what research is.

Then, from *tsugumi* to *tsugumu*, and by swapping its sounds (a metathesis), to *tsumugu*, to spin. The tool should help you spin your own research out of fragments of trial and error. The temari (a traditional thread ball) and the thread that the thrush holds in its beak, in the icon and on screen, come from this: a single thread drawn from the temari becomes the wire that connects Card to Card.

The other origin is the bird, the dusky thrush (*tsugumi* in Japanese). It comes to Japan as a winter bird, about 24 cm long, and is known for hopping a few steps on the ground, stopping, and standing up straight to look around. It does not dash through; it moves, stops, checks where it is, and moves on. That is also how research with AI should move.

One account says the bird got its name because it does not sing in Japan during winter, as if keeping its beak shut, *tsugumu* ([Suntory, "Encyclopedia of Japanese birds"](https://www.suntory.co.jp/eco/birds/encyclopedia/detail/1359.html)). However eloquently AI reports, Tsugumi should not rely on explanation in words alone, but let the process itself, what was actually tried and checked, speak. By following each step, working on it and running it again, researchers can build up their own conviction. In an age that tells results with ever more words, the tool quietly offers the accumulated attempts. That is the bearing of the thrush with its beak shut.

## The author

A first-year PhD student at the University of Tokyo, researching AI for astrophysics. Tsugumi grew out of what they had been thinking about in that research.

[Back to the README](../README.md#the-flow-of-processing-as-a-common-language-for-people-and-ai)
