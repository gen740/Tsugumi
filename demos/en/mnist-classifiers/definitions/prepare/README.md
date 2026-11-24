---
title: prepare
description: Make a stratified subset
---

With random numbers fixed by seed, draws train_size training images and test_size test images, keeping the share of each digit (stratified). The default counts keep the seven classifiers' run time within a minute. Scales pixels to 0–1, makes them 784-dimensional vectors, and passes them to every classifier in an npz file. Shows the count per digit.
