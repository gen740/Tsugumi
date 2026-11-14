"""Interpreter JSON adapters use the same IEEE vectors as the eight-SDK test."""
import json
import math
import os
from pathlib import Path
import struct
import subprocess

import pytest
from test_json_sdk_roles import executable

ROOT = Path(__file__).resolve().parents[2]
VECTORS = json.loads((ROOT / 'tests/fixtures/number-vectors.json').read_text())


def number_json():
    return '{"values":[' + ','.join(VECTORS['values']) + '],"strings":' + json.dumps(VECTORS['strings']) + ',"missing":null}'


@pytest.mark.parametrize('language', ['ruby', 'r', 'julia'])
def test_number_face_vectors(tmp_path, language):
    input_file = tmp_path / 'input.json'
    input_file.write_text(number_json())
    if language == 'ruby':
        script = tmp_path / 'test.rb'
        script.write_text('''require 'json'
require ARGV[0]
v=JSON.parse(File.read(ARGV[1]),allow_nan:true)
puts JSON.generate(Tsugumi.json_value(v),allow_nan:true)
''')
        args = [executable('RUBY'), str(script), str(ROOT / 'sdk/ruby/tsugumi.rb'), str(input_file)]
    elif language == 'julia':
        script = tmp_path / 'test.jl'
        script.write_text('''include(ARGS[1])
v=Tsugumi.parse_json(read(ARGS[2],String))
println(Tsugumi.json_text(v))
''')
        args = [executable('JULIA'), '--startup-file=no', '--history-file=no', str(script), str(ROOT / 'sdk/julia/tsugumi.jl'), str(input_file)]
    else:
        script = tmp_path / 'test.R'
        script.write_text('''source(commandArgs(TRUE)[[1]])
.Call <- function(name, ...) switch(name,tsugumi_params='{}',tsugumi_info='.',tsugumi_invocation=1)
scope <- new.env(); scope$active <- TRUE
.tsugumi_arguments(scope)
v <- .tsugumi_from_json(paste(readLines(commandArgs(TRUE)[[2]]),collapse=""))
cat(.tsugumi_json(v))
bad <- try(.tsugumi_json(list(value=NA_real_)),silent=TRUE)
stopifnot(inherits(bad,"try-error"))
'''.replace('.tsugumi_arguments(scope)\n', 'invisible(.tsugumi_arguments(scope))\n'))
        args = [executable('RSCRIPT'), '--vanilla', str(script), str(ROOT / 'sdk/r/tsugumi.R'), str(input_file)]
    result = subprocess.run(args, env={**os.environ, 'TSUGUMI_NATIVE_LIBRARY': 'unused', 'JULIA_DEPOT_PATH': str(tmp_path / 'depot')}, text=True, capture_output=True, check=True)
    value = json.loads(result.stdout)
    assert value['strings'] == VECTORS['strings']
    assert value['missing'] is None
    for actual, token in zip(value['values'], VECTORS['values'], strict=True):
        actual, expected = float(actual), float(token)
        assert math.isnan(actual) if math.isnan(expected) else struct.pack('<d', actual) == struct.pack('<d', expected)
