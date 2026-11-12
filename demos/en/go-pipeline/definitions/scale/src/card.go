package main

import "tsugumi"

func run(ctx *tsugumi.Context, input tsugumi.Input) (tsugumi.Output, error) {
	params, err := ctx.Params()
	if err != nil {
		return tsugumi.Output{}, err
	}
	product := input.Number.Value * *params.Factor
	if err := ctx.DisplayScalar(float64(product), "product"); err != nil {
		return tsugumi.Output{}, err
	}
	return tsugumi.Output{Out: &tsugumi.OutputOut{Value: product}}, nil
}

func onRunStart(ctx *tsugumi.Context) error { return nil }
