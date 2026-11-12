package main

import "tsugumi"

func run(ctx *tsugumi.Context, input tsugumi.Input) (tsugumi.Output, error) {
	if err := ctx.DisplayText("hello"); err != nil {
		return tsugumi.Output{}, err
	}
	return tsugumi.Output{Out: &tsugumi.OutputOut{Value: "hello"}}, nil
}

func onRunStart(ctx *tsugumi.Context) error { return nil }
