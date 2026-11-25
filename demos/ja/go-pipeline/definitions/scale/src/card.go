package main

import "tsugumi"

func run(inputs tsugumi.Inputs) (tsugumi.Output, error) {
	params := tsugumi.Parameters()
	product := inputs.Number().Value * uint64(params.Factor)
	if err := tsugumi.SendValue(tsugumi.Scalar{Value: float64(product), Label: "product"}); err != nil {
		return tsugumi.Output{}, err
	}
	return tsugumi.Output{Out: &tsugumi.OutputOut{Value: product}}, nil
}
