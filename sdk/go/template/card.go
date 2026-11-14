package main

import "tsugumi"

func run(inputs tsugumi.Inputs, report *tsugumi.Report) (tsugumi.Output, error) {
	if err := report.Text("hello"); err != nil {
		return tsugumi.Output{}, err
	}
	return tsugumi.Output{Out: &tsugumi.OutputOut{Value: "hello"}}, nil
}

func onRunStart(inputs tsugumi.Inputs) error { return nil }
