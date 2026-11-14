package main

import "tsugumi"

func run(inputs tsugumi.Inputs, report *tsugumi.Report) (tsugumi.Output, error) {
	params := inputs.Params()
	product := inputs.Number().Value * *params.Factor
	if err := report.Scalar(float64(product), "product"); err != nil {
		return tsugumi.Output{}, err
	}
	return tsugumi.Output{Out: &tsugumi.OutputOut{Value: product}}, nil
}

func onRunStart(inputs tsugumi.Inputs) error { return nil }
